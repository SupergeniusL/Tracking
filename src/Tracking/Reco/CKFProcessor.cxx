#include "Tracking/Reco/CKFProcessor.h"

#include "SimCore/Event/SimParticle.h"
#include "Tracking/Sim/GeometryContainers.h"

//--- C++ StdLib ---//
#include <algorithm>  //std::vector reverse
#include <iostream>

// eN files
#include <fstream>

namespace tracking {
namespace reco {

CKFProcessor::CKFProcessor(const std::string& name, framework::Process& process)
    : framework::Producer(name, process) {
  normal_ = std::make_shared<std::normal_distribution<float>>(0., 1.);
}

CKFProcessor::~CKFProcessor() {}

void CKFProcessor::onProcessStart() {
  profiling_map_["setup"] = 0.;
  profiling_map_["hits"] = 0.;
  profiling_map_["seeds"] = 0.;
  profiling_map_["ckf_setup"] = 0.;
  profiling_map_["ckf_run"] = 0.;
  profiling_map_["result_loop"] = 0.;

  gctx_ = Acts::GeometryContext();
  bctx_ = Acts::MagneticFieldContext();

  // Build the tracking geometry
  ldmx_tg = std::make_shared<tracking::reco::TrackersTrackingGeometry>(
      //"/Users/pbutti/sw/ldmx-sw/Detectors/data/ldmx-det-v12/detector.gdml",
      "detector.gdml",
      &gctx_, debug_);
  const auto tGeometry = ldmx_tg->getTG();

  if (dumpobj_) ldmx_tg->dumpGeometry("./");

  // Seed the generator
  generator_.seed(1);

  //==> Move to a separate processor <== //

  // Generate a constant magnetic field
  Acts::Vector3 b_field(0., 0., bfield_ * Acts::UnitConstants::T);

  // Setup a constant magnetic field
  const auto constBField = std::make_shared<Acts::ConstantBField>(b_field);

  // TODO:: Move this to an external file
  auto localToGlobalBin_xyz = [](std::array<size_t, 3> bins,
                                 std::array<size_t, 3> sizes) {
    return (bins[0] * (sizes[1] * sizes[2]) + bins[1] * sizes[2] +
            bins[2]);  // xyz - field space
    // return (bins[1] * (sizes[2] * sizes[0]) + bins[2] * sizes[0] + bins[0]);
    // //zxy
  };

  // Setup a interpolated bfield map
  const auto map = std::make_shared<InterpolatedMagneticField3>(
      makeMagneticFieldMapXyzFromText(
          std::move(localToGlobalBin_xyz), field_map_,
          1. * Acts::UnitConstants::mm,    // default scale for axes length
          1000. * Acts::UnitConstants::T,  // The map is in kT, so scale it to T
          false,                           // not symmetrical
          true                             // rotate the axes to tracking frame
          ));

  // Setup the steppers
  const auto stepper = Acts::EigenStepper<>{map};
  const auto const_stepper = Acts::EigenStepper<>{constBField};
  const auto multi_stepper = Acts::MultiEigenStepperLoop{map};

  // Setup the navigator
  Acts::Navigator::Config navCfg{tGeometry};
  navCfg.resolveMaterial = true;
  navCfg.resolvePassive = true;
  navCfg.resolveSensitive = true;
  const Acts::Navigator navigator(navCfg);

  // Setup the propagators
  propagator_ = const_b_field_
                    ? std::make_unique<CkfPropagator>(const_stepper, navigator)
                    : std::make_unique<CkfPropagator>(stepper, navigator);
  auto gsf_propagator = GsfPropagator(multi_stepper, navigator);

  // Setup the fitters
  ckf_ = std::make_unique<std::decay_t<decltype(*ckf_)>>(*propagator_);
  kf_ = std::make_unique<std::decay_t<decltype(*kf_)>>(*propagator_);
  gsf_ = std::make_unique<std::decay_t<decltype(*gsf_)>>(
      std::move(gsf_propagator));

  // Setup the propagator steps writer
  tracking::sim::PropagatorStepWriter::Config cfg;
  cfg.filePath = steps_outfile_path_;

  writer_ = std::make_unique<tracking::sim::PropagatorStepWriter>(cfg);

  
}

void CKFProcessor::produce(framework::Event& event) {

  eventnr_++;

  //if (eventnr_ != 8283)
  //  return;
  
  // int counter=0
  // if (counter==0) {
  // propagateENstates(event,
  //                   "hadron_vars_piminus_1e6.txt",
  //                   "hadron_vars_piminus_1e6_out.root");
  // counter+=1;
  // }

  // TODO use global variable instead and call clear;
  std::vector<ldmx::Track> tracks;

  auto start = std::chrono::high_resolution_clock::now();

  nevents_++;
  if (nevents_ % 1000 == 0)
    ldmx_log(info) << "events processed:" << nevents_;

  /*
  std::shared_ptr<const Acts::PerigeeSurface> perigee_surface =
      Acts::Surface::makeShared<Acts::PerigeeSurface>(
          Acts::Vector3(perigee_location_.at(0), perigee_location_.at(1),
                        perigee_location_.at(2)));
  */
                        
  auto loggingLevel = Acts::Logging::DEBUG;
  ACTS_LOCAL_LOGGER(
      Acts::getDefaultLogger("LDMX Tracking Goemetry Maker", loggingLevel));

  Acts::PropagatorOptions<ActionList, AbortList> propagator_options(
      gctx_, bctx_, Acts::LoggerWrapper{logger()});

  propagator_options.pathLimit = std::numeric_limits<double>::max();

  // Activate loop protection at some pt value
  propagator_options.loopProtection =
      false;  //(startParameters.transverseMomentum() < cfg.ptLoopers);

  // Switch the material interaction on/off & eventually into logging mode
  auto& mInteractor =
      propagator_options.actionList.get<Acts::MaterialInteractor>();
  mInteractor.multipleScattering = true;
  mInteractor.energyLoss = true;
  mInteractor.recordInteractions = false;

  // The logger can be switched to sterile, e.g. for timing logging
  auto& sLogger =
      propagator_options.actionList.get<Acts::detail::SteppingLogger>();
  sLogger.sterile = true;
  // Set a maximum step size
  propagator_options.maxStepSize =
      propagator_step_size_ * Acts::UnitConstants::mm;
  propagator_options.maxSteps = propagator_maxSteps_;

  // Electron hypothesis
  propagator_options.mass = 0.511 * Acts::UnitConstants::MeV;

  // std::cout<<"Setting up the Kalman Filter Algorithm"<<std::endl;

  // #######################//
  // Kalman Filter algorithm//
  // #######################//

  // Step 1 - Form the source links

  // std::vector<ActsExamples::IndexSourceLink> sourceLinks;
  // a) Loop over the sim Hits

  auto setup = std::chrono::high_resolution_clock::now();
  profiling_map_["setup"] +=
      std::chrono::duration<double, std::milli>(setup - start).count();

  const std::vector<ldmx::SimTrackerHit> sim_hits =
      event.getCollection<ldmx::SimTrackerHit>(hit_collection_);
  

  const std::vector<ldmx::Measurement> measurements =
      event.getCollection<ldmx::Measurement>(measurement_collection_);
  

  // The mapping between the geometry identifier
  // and the IndexsourceLink that points to the hit
  const auto geoId_sl_map = makeGeoIdSourceLinkMap(measurements);

  auto hits = std::chrono::high_resolution_clock::now();
  profiling_map_["hits"] +=
      std::chrono::duration<double, std::milli>(hits - setup).count();

  // ============   Setup the CKF  ============

  // Retrieve the seeds

  ldmx_log(debug) << "Retrieve the seeds::" << seed_coll_name_;
  

  const std::vector<ldmx::Track> seed_tracks =
      event.getCollection<ldmx::Track>(seed_coll_name_);

  // Run the CKF on each seed and produce a track candidate
  std::vector<Acts::BoundTrackParameters> startParameters;
  for (auto& seed : seed_tracks) {

    // Transform the seed track to bound parameters
    std::shared_ptr<Acts::PerigeeSurface> perigeeSurface =
        Acts::Surface::makeShared<Acts::PerigeeSurface>(Acts::Vector3(
            seed.getPerigeeX(), seed.getPerigeeY(), seed.getPerigeeZ()));

    Acts::BoundVector paramVec;
    paramVec << seed.getD0(), seed.getZ0(), seed.getPhi(), seed.getTheta(),
        seed.getQoP(), seed.getT();

    Acts::BoundSymMatrix covMat =
        tracking::sim::utils::unpackCov(seed.getPerigeeCov());

    ldmx_log(debug) 
        << "perigee" << std::endl
        << seed.getPerigeeX() << " " << seed.getPerigeeY() << " "
        << seed.getPerigeeZ() << std::endl
        << "start Parameters" << std::endl
        << paramVec;
    
    Acts::ActsScalar q = seed.getQoP() < 0 ? -1 * Acts::UnitConstants::e
                                           : Acts::UnitConstants::e;

    startParameters.push_back(
        Acts::BoundTrackParameters(perigeeSurface, paramVec, q, covMat));
  }

  if (startParameters.size() != 1) {
    std::vector<ldmx::Track> empty;
    event.add(out_trk_collection_, empty);
    return;
  }

  nseeds_++;

  
  auto seeds = std::chrono::high_resolution_clock::now();
  profiling_map_["seeds"] +=
      std::chrono::duration<double, std::milli>(seeds - hits).count();

  Acts::GainMatrixUpdater kfUpdater;
  Acts::GainMatrixSmoother kfSmoother;

  // configuration for the measurement selector. Empty geometry identifier means
  // applicable to all the detector elements

  Acts::MeasurementSelector::Config measurementSelectorCfg = {
      // global default: no chi2 cut, only one measurement per surface
      {Acts::GeometryIdentifier(),
       {{}, {std::numeric_limits<double>::max()}, {1u}}},
  };

  Acts::MeasurementSelector measSel{measurementSelectorCfg};
  
  tracking::sim::LdmxMeasurementCalibrator calibrator{measurements};

  Acts::CombinatorialKalmanFilterExtensions ckf_extensions;
  
  if (use1Dmeasurements_)
    ckf_extensions.calibrator.connect<&tracking::sim::LdmxMeasurementCalibrator::calibrate_1d>(
        &calibrator);
  else
    ckf_extensions.calibrator.connect<&tracking::sim::LdmxMeasurementCalibrator::calibrate>(
        &calibrator);
  
  ckf_extensions.updater.connect<&Acts::GainMatrixUpdater::operator()>(
      &kfUpdater);
  ckf_extensions.smoother.connect<&Acts::GainMatrixSmoother::operator()>(
      &kfSmoother);
  ckf_extensions.measurementSelector
      .connect<&Acts::MeasurementSelector::select>(&measSel);


  ldmx_log(debug) 
      << "SourceLinkAccessor..." <<  std::endl;
  
  // Create source link accessor and connect delegate
  struct SourceLinkAccIt {
    using BaseIt = decltype(geoId_sl_map.begin());
    BaseIt it;

    using difference_type = typename BaseIt::difference_type;
    using iterator_category = typename BaseIt::iterator_category;
    using value_type = typename BaseIt::value_type::second_type;
    using pointer = typename BaseIt::pointer;
    using reference = value_type&;

    SourceLinkAccIt& operator++() {
      ++it;
      return *this;
    }
    bool operator==(const SourceLinkAccIt& other) const {
      return it == other.it;
    }
    bool operator!=(const SourceLinkAccIt& other) const {
      return !(*this == other);
    }
    const value_type& operator*() const { return it->second; }
  };

  auto sourceLinkAccessor = [&](const Acts::Surface& surface)
                            -> std::pair<SourceLinkAccIt, SourceLinkAccIt> {
    auto [begin, end] = geoId_sl_map.equal_range(surface.geometryId());
    return {SourceLinkAccIt{begin}, SourceLinkAccIt{end}};
  };
  
  Acts::SourceLinkAccessorDelegate<SourceLinkAccIt> sourceLinkAccessorDelegate;
  sourceLinkAccessorDelegate.connect<&decltype(sourceLinkAccessor)::operator(),
                                     decltype(sourceLinkAccessor)>(
                                         &sourceLinkAccessor);
  
  ldmx_log(debug) 
      << "Surfaces..." <<  std::endl;
  
  std::shared_ptr<const Acts::PerigeeSurface> origin_surface =
      Acts::Surface::makeShared<Acts::PerigeeSurface>(
          Acts::Vector3(0., 0., 0.));

  auto extr_surface = &(*origin_surface);

  std::shared_ptr<const Acts::PerigeeSurface> tgt_surface =
      Acts::Surface::makeShared<Acts::PerigeeSurface>(
          Acts::Vector3(extrapolate_location_[0], extrapolate_location_[1],
                        extrapolate_location_[2]));

  if (use_extrapolate_location_) {
    extr_surface = &(*tgt_surface);
  }

  Acts::Vector3 seed_perigee_surface_center =
      startParameters.at(0).referenceSurface().center(gctx_);
  std::shared_ptr<const Acts::PerigeeSurface> seed_surface =
      Acts::Surface::makeShared<Acts::PerigeeSurface>(
          seed_perigee_surface_center);

  if (use_seed_perigee_) {
    extr_surface = &(*seed_surface);
  }

  auto ckf_loggingLevel = Acts::Logging::FATAL;
  if (debug_)
    ckf_loggingLevel = Acts::Logging::VERBOSE;
  const auto ckflogger = Acts::getDefaultLogger("CKF", ckf_loggingLevel);
  const Acts::CombinatorialKalmanFilterOptions<SourceLinkAccIt> ckfOptions(
      gctx_, bctx_, cctx_, sourceLinkAccessorDelegate, ckf_extensions,
      Acts::LoggerWrapper{*ckflogger}, propagator_options, &(*extr_surface));


  const Acts::CombinatorialKalmanFilterOptions<SourceLinkAccIt> ckfOptions_meas(
      gctx_, bctx_, cctx_, sourceLinkAccessorDelegate, ckf_extensions,
      Acts::LoggerWrapper{*ckflogger}, propagator_options, &(*extr_surface));


  ldmx_log(debug) 
      << "About to run CKF..." <<  std::endl;
    
  // run the CKF for all initial track states
  auto ckf_setup = std::chrono::high_resolution_clock::now();
  profiling_map_["ckf_setup"] +=
      std::chrono::duration<double, std::milli>(ckf_setup - seeds).count();

  auto results = ckf_->findTracks(startParameters, ckfOptions_meas);
  
  ldmx_log(debug) 
      << "CKF Ran!" <<  std::endl;
    
  auto ckf_run = std::chrono::high_resolution_clock::now();
  profiling_map_["ckf_run"] +=
      std::chrono::duration<double, std::milli>(ckf_run - ckf_setup).count();

  // std::cout<<"StartParameters size::"<<startParameters.size()<<std::endl;
  // std::cout<<"results size::"<<results.size()<<std::endl;

  int ResultIndex = 0;
  int GoodResult = 0;

  for (auto& result : results) {
    ResultIndex++;
    if (!result.ok()) {
      continue;
    }
    GoodResult++;

    auto ckf_result = result.value();

    
    ldmx_log(debug) 
        << "Track Index::" << GoodResult - 1 << std::endl
        << "===============" << std::endl
        << "ckf_result:: filtered " << ckf_result.filtered << std::endl
        << "ckf_result:: smoothed " << ckf_result.smoothed << std::endl
        << "ckf_result:: iSmoothed " << ckf_result.iSmoothed << std::endl
        << "ckf_result:: finished " << ckf_result.finished << std::endl
        << "Size of the active Tips " << ckf_result.activeTips.size() << std::endl
        << "Last Measurement indices "
        << ckf_result.lastMeasurementIndices.size();
    for (auto& lm : ckf_result.lastMeasurementIndices) {
      ldmx_log(debug)<< "LastMeasurementIndex=" << lm;
    }
    ldmx_log(debug) << "Last Track indices " << ckf_result.lastTrackIndices.size();
    
    for (auto& lt : ckf_result.lastTrackIndices) {
      ldmx_log(debug) << "LastTrackIndex=" << lt;
    }
    
    
    // The track tips are the last measurement index
    Acts::MultiTrajectory mj = ckf_result.fittedStates;

    // In the current case I should have a single trackTip
    // Check
    // https://github.com/acts-project/acts/blob/8f1f47bb57044b3e476d01b3dbafb13030038bd5/Examples/Io/Performance/ActsExamples/Io/Performance/TrackFitterPerformanceWriter.cpp#L114
    auto trackTip = ckf_result.lastMeasurementIndices.front();

    // Collect the trajectory summary info
    auto trajState =
        Acts::MultiTrajectoryHelpers::trajectoryState(mj, trackTip);

    // Check some track info

    ldmx_log(debug)
        << "nStates=" << trajState.nStates << std::endl
        << "nMeasurements=" << trajState.nMeasurements << std::endl
        << "nOutliers=" << trajState.nOutliers << std::endl
        << "nHoles=" << trajState.nHoles << std::endl
        << "chi2sum=" << trajState.chi2Sum << std::endl
        << "NDF=" << trajState.NDF << std::endl
        << "nsharedHits=" << trajState.nSharedHits;
        
        
    if (trajState.nStates !=
        trajState.nMeasurements + trajState.nOutliers + trajState.nHoles) {
      ldmx_log(warn)<<"Found track with nStates inconsistent with expectation";
      // continue;
    }

    // Cut on number of hits?
    if (trajState.nMeasurements < min_hits_) continue;

    // Create a track object

    ldmx::Track trk = ldmx::Track();
    trk.setPerigeeLocation(extr_surface->center(gctx_)[0],
                           extr_surface->center(gctx_)[1],
                           extr_surface->center(gctx_)[2]);

    trk.setChi2(trajState.chi2Sum);
    trk.setNhits(trajState.nMeasurements);
    trk.setNdf(trajState.NDF);
    trk.setNsharedHits(trajState.nSharedHits);

    // Set the perigee parameters (TODO:: There should only be one single track
    // per finding-fit for now. This method needs to be updated)
    // trk.setPerigeeParameters(ckf_result.fittedParameters.begin()->second.parameters());

    Acts::BoundVector tgt_srf_pars =
        ckf_result.fittedParameters.begin()->second.parameters();
    Acts::BoundMatrix tgt_srf_cov =
        ckf_result.fittedParameters.begin()->second.covariance()
            ? ckf_result.fittedParameters.begin()->second.covariance().value()
            : Acts::BoundSymMatrix::Identity();
    
    /*
    if (ckf_result.fittedParameters.begin()->second.charge() > 0) {
      std::cout<<getName()<<"::ERROR!!! ERROR!! Found track with q>0.
    Chi2="<<trajState.chi2Sum<<std::endl; mj.visitBackwards(trackTip, [&](const
    auto& state) { std::cout<<"Printing smoothed states"<<std::endl;
        std::cout<<state.smoothed()<<std::endl;
        std::cout<<"Printing filtered states"<<std::endl;
        std::cout<<state.filtered()<<std::endl;
      });


    }*/

    std::vector<double> v_tgt_srf_pars(
        tgt_srf_pars.data(),
        tgt_srf_pars.data() + tgt_srf_pars.rows() * tgt_srf_pars.cols());
    std::vector<double> v_tgt_srf_cov_flat;
    tracking::sim::utils::flatCov(tgt_srf_cov, v_tgt_srf_cov_flat);
    Acts::Vector3 trk_momentum =
        ckf_result.fittedParameters.begin()->second.momentum();
    Acts::Vector3 trk_position =
        ckf_result.fittedParameters.begin()->second.position(gctx_);

    trk.setPerigeeParameters(v_tgt_srf_pars);
    trk.setPerigeeCov(v_tgt_srf_cov_flat);
    trk.setMomentum(trk_momentum(0), trk_momentum(1), trk_momentum(2));
    trk.setPosition(trk_position(0), trk_position(1), trk_position(2));

    tracks.push_back(trk);
    ntracks_++;

    // Write the event display for the recoil
    //if (ckf_result.fittedParameters.begin()->second.absoluteMomentum() < 1.2 &&
    //    false) {
    //  writeEvent(event, ckf_result.fittedParameters.begin()->second, mj,
    //             trackTip, measurements);
    //}

    // Refit the track with the KalmanFitter using backward propagation
    if (kf_refit_) {
      std::cout << "Preparing theKF refit" << std::endl;
      std::vector<std::reference_wrapper<const ActsExamples::IndexSourceLink>>
          fit_trackSourceLinks;
      mj.visitBackwards(trackTip, [&](const auto& state) {
        const auto& sourceLink =
            static_cast<const ActsExamples::IndexSourceLink&>(
                state.uncalibrated());
        auto typeFlags = state.typeFlags();
        if (typeFlags.test(Acts::TrackStateFlag::MeasurementFlag)) {
          fit_trackSourceLinks.push_back(std::cref(sourceLink));
        }
      });

      std::cout << "Getting the logger and adding the extensions." << std::endl;
      const auto kfLogger =
          Acts::getDefaultLogger("KalmanFitter", Acts::Logging::INFO);
      Acts::KalmanFitterExtensions kfitter_extensions;
      kfitter_extensions.calibrator
          .connect<&tracking::sim::LdmxMeasurementCalibrator::calibrate_1d>(&calibrator);
      kfitter_extensions.updater.connect<&Acts::GainMatrixUpdater::operator()>(
          &kfUpdater);
      kfitter_extensions.smoother
          .connect<&Acts::GainMatrixSmoother::operator()>(&kfSmoother);

      // rFiltering is true, so it should run in reversed direction.
      Acts::KalmanFitterOptions kfitter_options = Acts::KalmanFitterOptions(
          gctx_, bctx_, cctx_, kfitter_extensions,
          Acts::LoggerWrapper{*kfLogger}, propagator_options, &(*extr_surface),
          true, true, true);  // mScattering, exoLoss, rFiltering

      // Acts::MagneticFieldProvider::Cache cache =
      // sp_interpolated_bField_->makeCache(bctx_); std::cout<<"
      // BField::\n"<<sp_interpolated_bField_->getField(ckf_result.fittedParameters.begin()->second.position(gctx_),cache).value()
      // / Acts::UnitConstants::T <<std::endl;

      auto kf_refit_result = kf_->fit(
          fit_trackSourceLinks.begin(), fit_trackSourceLinks.end(),
          ckf_result.fittedParameters.begin()->second, kfitter_options);

      if (!kf_refit_result.ok()) {
        std::cout << "KF Refit failed" << std::endl;
      } else {}

    }  // Run the refit

    // Refit track using the GSF
    if (gsf_refit_) {
      try {
        const auto gsfLogger =
            Acts::getDefaultLogger("GSF", Acts::Logging::INFO);
        std::vector<std::reference_wrapper<const ActsExamples::IndexSourceLink>>
            fit_trackSourceLinks;
        mj.visitBackwards(trackTip, [&](const auto& state) {
          auto typeFlags = state.typeFlags();
          if (typeFlags.test(Acts::TrackStateFlag::MeasurementFlag)) {
            const auto& sourceLink =
                static_cast<const ActsExamples::IndexSourceLink&>(
                    state.uncalibrated());
            fit_trackSourceLinks.push_back(std::cref(sourceLink));
          }
        });

        // Same extensions of the KF
        Acts::GsfExtensions gsf_extensions;
        gsf_extensions.calibrator
            .connect<&tracking::sim::LdmxMeasurementCalibrator::calibrate_1d>(&calibrator);
        gsf_extensions.updater.connect<&Acts::GainMatrixUpdater::operator()>(
            &kfUpdater);

        Acts::GsfOptions gsf_options{gctx_,
                                     bctx_,
                                     cctx_,
                                     gsf_extensions,
                                     Acts::LoggerWrapper{*gsfLogger},
                                     propagator_options,
                                     &(*extr_surface)};

        gsf_options.abortOnError = false;
        gsf_options.maxComponents = 4;
        gsf_options.disableAllMaterialHandling = false;

        auto gsf_refit_result =
            gsf_->fit(fit_trackSourceLinks.begin(), fit_trackSourceLinks.end(),
                      ckf_result.fittedParameters.begin()->second, gsf_options);

        if (!gsf_refit_result.ok()) {
          std::cout << "GSF Refit failed" << std::endl;
        } else {}

      } catch (...) {
        std::cout << "ERROR:: GSF Refit failed" << std::endl;
      }
    }  // do refit GSF
  }    // loop on CKF Results

  auto result_loop = std::chrono::high_resolution_clock::now();
  profiling_map_["result_loop"] +=
      std::chrono::duration<double, std::milli>(result_loop - ckf_run).count();
  
  // Add the tracks to the event
  event.add(out_trk_collection_, tracks);

  auto end = std::chrono::high_resolution_clock::now();
  // long long microseconds =
  // std::chrono::duration_cast<std::chrono::microseconds>(end-start).count();
  auto diff = end - start;
  processing_time_ += std::chrono::duration<double, std::milli>(diff).count();
}

void CKFProcessor::onProcessEnd() {
  std::cout << "Producer " << getName() << " found " << ntracks_
            << " tracks  / " << nseeds_ << " nseeds" << std::endl;

  
  std::cout << "PROCESSOR:: " << this->getName()
            << "   AVG Time/Event: " << processing_time_ / nevents_ << " ms"
            << std::endl;

  std::cout << "Breakdown::" << std::endl;
  std::cout << "setup       Avg Time/Event = "
            << profiling_map_["setup"] / nevents_ << " ms" << std::endl;
  std::cout << "hits        Avg Time/Event = "
            << profiling_map_["hits"] / nevents_ << " ms" << std::endl;
  std::cout << "seeds       Avg Time/Event = "
            << profiling_map_["seeds"] / nevents_ << " ms" << std::endl;
  std::cout << "cf_setup    Avg Time/Event = "
            << profiling_map_["ckf_setup"] / nevents_ << " ms" << std::endl;
  std::cout << "ckf_run     Avg Time/Event = "
            << profiling_map_["ckf_run"] / nevents_ << " ms" << std::endl;
  std::cout << "result_loop Avg Time/Event = "
            << profiling_map_["result_loop"] / nevents_ << " ms" << std::endl;
}

void CKFProcessor::configure(framework::config::Parameters& parameters) {
  dumpobj_ = parameters.getParameter<bool>("dumpobj", 0);
  pionstates_ = parameters.getParameter<int>("pionstates", 0);
  steps_outfile_path_ = parameters.getParameter<std::string>(
      "steps_file_path", "propagation_steps.root");
  track_id_ = parameters.getParameter<int>("track_id", -1);
  pdg_id_ = parameters.getParameter<int>("pdg_id", 11);

  bfield_ = parameters.getParameter<double>("bfield", 0.);
  const_b_field_ = parameters.getParameter<bool>("const_b_field", true);
  field_map_ = parameters.getParameter<std::string>("field_map");
  propagator_step_size_ =
      parameters.getParameter<double>("propagator_step_size", 200.);
  propagator_maxSteps_ =
      parameters.getParameter<int>("propagator_maxSteps", 10000);
  perigee_location_ = parameters.getParameter<std::vector<double>>(
      "perigee_location", {0., 0., 0.});
  hit_collection_ =
      parameters.getParameter<std::string>("hit_collection", "TaggerSimHits");
  measurement_collection_ =
      parameters.getParameter<std::string>("measurement_collection","TaggerMeasurements");
  
  remove_stereo_ = parameters.getParameter<bool>("remove_stereo", false);
  if (remove_stereo_)
    std::cout << "CONFIGURE::remove_stereo=" << (int)remove_stereo_ << std::endl;

  use1Dmeasurements_ = parameters.getParameter<bool>("use1Dmeasurements", true);

  if (use1Dmeasurements_)
    std::cout << "CONFIGURE::use1Dmeasurements=" << (int)use1Dmeasurements_
              << std::endl;

  min_hits_ = parameters.getParameter<int>("min_hits", 7);

  std::cout << "CONFIGURE::min_hits=" << min_hits_ << std::endl;

  // Ckf specific options
  use_extrapolate_location_ =
      parameters.getParameter<bool>("use_extrapolate_location", true);
  extrapolate_location_ = parameters.getParameter<std::vector<double>>(
      "extrapolate_location", {0., 0., 0.});
  use_seed_perigee_ = parameters.getParameter<bool>("use_seed_perigee", false);

  // seeds from the event
  seed_coll_name_ =
      parameters.getParameter<std::string>("seed_coll_name", "seedTracks");

  // output track collection
  out_trk_collection_ =
      parameters.getParameter<std::string>("out_trk_collection", "Tracks");

  // Hit smearing

  do_smearing_ = parameters.getParameter<bool>("do_smearing", false);
  sigma_u_ = parameters.getParameter<double>("sigma_u", 0.01);
  sigma_v_ = parameters.getParameter<double>("sigma_v", 0.);

  std::cout << __PRETTY_FUNCTION__ << "  HitCollection::" << hit_collection_
            << std::endl;

  kf_refit_ = parameters.getParameter<bool>("kf_refit", false);
  gsf_refit_ = parameters.getParameter<bool>("gsf_refit", false);
}

void CKFProcessor::testField(
    const std::shared_ptr<Acts::MagneticFieldProvider> bfield,
    const Acts::Vector3& eval_pos) const {
  Acts::MagneticFieldProvider::Cache cache = bfield->makeCache(bctx_);
  std::cout << "Pos::\n" << eval_pos << std::endl;
  std::cout << " BField::\n"
            << bfield->getField(eval_pos, cache).value() /
                   Acts::UnitConstants::T
            << std::endl;
}

void CKFProcessor::testMeasurmentCalibrator(
    const tracking::sim::LdmxMeasurementCalibrator& calibrator,
    const std::unordered_map<Acts::GeometryIdentifier,
                             std::vector<ActsExamples::IndexSourceLink>>& map)
    const {
  for (const auto& pair : map) {
    std::cout << "GeometryID::" << pair.first << std::endl;
    for (auto& sl : pair.second) {
      calibrator.test(gctx_, sl);
    }
  }
}

// This functioon takes the input parameters and makes the propagation for a
// simple event display
void CKFProcessor::writeEvent(
    framework::Event& event,
    const Acts::BoundTrackParameters& perigeeParameters,
    const Acts::MultiTrajectory& mj, const int& trackTip,
    const std::vector<ldmx::Measurement> measurements) {
  // Prepare the outputs..
  std::vector<std::vector<Acts::detail::Step>> propagationSteps;
  propagationSteps.reserve(1);

  /// Using some short hands for Recorded Material
  using RecordedMaterial = Acts::MaterialInteractor::result_type;

  /// And recorded material track
  /// - this is start:  position, start momentum
  ///   and the Recorded material
  using RecordedMaterialTrack =
      std::pair<std::pair<Acts::Vector3, Acts::Vector3>, RecordedMaterial>;

  /// Finally the output of the propagation test
  using PropagationOutput =
      std::pair<std::vector<Acts::detail::Step>, RecordedMaterial>;

  PropagationOutput pOutput;
  const auto evtLogger =
      Acts::getDefaultLogger("evtDisplay", Acts::Logging::INFO);
  Acts::PropagatorOptions<ActionList, AbortList> propagator_options(
      gctx_, bctx_, Acts::LoggerWrapper{*evtLogger});

  propagator_options.pathLimit = std::numeric_limits<double>::max();

  // Activate loop protection at some pt value
  propagator_options.loopProtection =
      false;  //(startParameters.transverseMomentum() < cfg.ptLoopers);

  // Switch the material interaction on/off & eventually into logging mode
  auto& mInteractor =
      propagator_options.actionList.get<Acts::MaterialInteractor>();
  mInteractor.multipleScattering = true;
  mInteractor.energyLoss = true;
  mInteractor.recordInteractions = false;

  // The logger can be switched to sterile, e.g. for timing logging
  auto& sLogger =
      propagator_options.actionList.get<Acts::detail::SteppingLogger>();
  sLogger.sterile = false;
  // Set a maximum step size
  propagator_options.maxStepSize =
      propagator_step_size_ * Acts::UnitConstants::mm;
  propagator_options.maxSteps = propagator_maxSteps_;

  // Loop over the states and the surfaces of the multi-trajectory and get
  // the arcs of helix from start to next surface

  std::vector<Acts::BoundTrackParameters> prop_parameters;
  // std::vector<std::reference_wrapper<const Acts::Surface>> ref_surfaces;
  std::vector<std::reference_wrapper<const ActsExamples::IndexSourceLink>>
      sourceLinks;

  mj.visitBackwards(trackTip, [&](const auto& state) {
    auto typeFlags = state.typeFlags();

    // Only store the track states for each measurement
    if (typeFlags.test(Acts::TrackStateFlag::MeasurementFlag)) {
      const auto& surface = state.referenceSurface();
      // ref_surfaces.push_back(surface);
      Acts::BoundVector smoothed = state.smoothed();
      // auto cov = state.smoothedCovariance;

      Acts::ActsScalar q = smoothed[Acts::eBoundQOverP] < 0
                               ? -1 * Acts::UnitConstants::e
                               : Acts::UnitConstants::e;

      prop_parameters.push_back(
          Acts::BoundTrackParameters(surface.getSharedPtr(), smoothed, q));
    }
  });

  // Reverse the parameters to start from the target
  std::reverse(prop_parameters.begin(), prop_parameters.end());

  // This holds all the steps to be merged
  std::vector<std::vector<Acts::detail::Step>> tmpSteps;
  tmpSteps.reserve(prop_parameters.size());

  
  // Start from the first parameters
  // Propagate to next surface
  // Grab the next parameters
  // Propagate to the next surface..
  // The last parameters just propagate
  // TODO Fix double code
  // TODO Fix contorted code - directly push to the same vector

  std::vector<Acts::detail::Step> steps;

  // compute first the perigee to first surface:
  auto result = propagator_->propagate(perigeeParameters,
                                       prop_parameters.at(0).referenceSurface(),
                                       propagator_options);

  if (result.ok()) {
    const auto& resultValue = result.value();
    auto steppingResults =
        resultValue.template get<Acts::detail::SteppingLogger::result_type>();
    // Set the stepping result
    pOutput.first = std::move(steppingResults.steps);

    for (auto& step : pOutput.first) steps.push_back(std::move(step));
  }

  // follow now the trajectory

  for (int i_params = 0; i_params < prop_parameters.size(); i_params++) {
    if (i_params < prop_parameters.size() - 1) {
      auto result = propagator_->propagate(
          prop_parameters.at(i_params),
          prop_parameters.at(i_params + 1).referenceSurface(),
          propagator_options);

      if (result.ok()) {
        const auto& resultValue = result.value();
        auto steppingResults =
            resultValue
                .template get<Acts::detail::SteppingLogger::result_type>();
        // Set the stepping result
        pOutput.first = std::move(steppingResults.steps);

        for (auto& step : pOutput.first) steps.push_back(std::move(step));

        // Record the propagator steps
        // tmpSteps.push_back(std::move(pOutput.first));
      }
    }

    // propagation for the last state
    else {
      auto result = propagator_->propagate(prop_parameters.at(i_params),
                                           propagator_options);

      if (result.ok()) {
        const auto& resultValue = result.value();
        auto steppingResults =
            resultValue
                .template get<Acts::detail::SteppingLogger::result_type>();
        // Set the stepping result
        pOutput.first = std::move(steppingResults.steps);

        for (auto& step : pOutput.first) steps.push_back(std::move(step));

        // Record the propagator steps
        // tmpSteps.push_back(std::move(pOutput.first));
      }
    }
  }

  // This holds all the steps to be merged
  // TODO Remove this and put all in the same vector directly in the for loop
  // above

  /*

  int totalSize=0;

  for (auto & steps: tmpSteps)
    totalSize+=steps.size();

    allSteps.reserve(totalSize);

  for (auto & steps: tmpSteps)
    allSteps.insert(allSteps.end(), steps.begin(), steps.end());
  */

  propagationSteps.push_back(steps);
  Acts::Vector3 gen_pos(0., 0., 0.);
  Acts::Vector3 gen_mom(0., 0., 0.);
  writer_->WriteSteps(event, propagationSteps, measurements, gen_pos, gen_mom);
}


auto CKFProcessor::makeGeoIdSourceLinkMap(
    const std::vector<ldmx::Measurement>& measurements)
    -> std::unordered_multimap<Acts::GeometryIdentifier,
                               ActsExamples::IndexSourceLink> {
  std::unordered_multimap<Acts::GeometryIdentifier,
                          ActsExamples::IndexSourceLink>
      geoId_sl_map;

  // Check the hits associated to the surfaces
  for (unsigned int i_meas = 0; i_meas < measurements.size(); i_meas++) {
    ldmx::Measurement meas = measurements.at(i_meas);
    unsigned int layerid = meas.getLayerID();

    const Acts::Surface* hit_surface = ldmx_tg->getSurface(layerid);

    if (hit_surface) {
      // Transform the ldmx space point from global to local and store the
      // information

      ActsExamples::IndexSourceLink idx_sl(hit_surface->geometryId(),
                                           i_meas);
      
      geoId_sl_map.insert(std::make_pair(hit_surface->geometryId(), idx_sl));

    } else
      std::cout << getName() << "::HIT " << i_meas << " at layer"
                << (measurements.at(i_meas)).getLayerID()
                << " is not associated to any surface?!" << std::endl;
  }
  
  return geoId_sl_map;
}


// This is used to propagate the initial eN states through the detector

void CKFProcessor::propagateENstates(framework::Event& event,
                                     std::string inputFile,
                                     std::string outFile) {
  std::ifstream inFile(inputFile);
  if (!inFile.is_open()) {
    std::cout << __PRETTY_FUNCTION__ << " could not read input file "
              << inputFile << std::endl;
    return;
  }

  // skip first line
  std::string spdg, smass, sE, spx, spy, spz, sp;
  int pdg;
  float mass, E, px, py, pz, p;
  std::getline(inFile, spdg);

  Acts::Vector3 gen_pos{0., 0., 0.};
  Acts::Vector3 gen_mom{0., 0., 0.};

  int total = 0;
  int pzCut = 0;
  int pCut = 0;
  int success = 0;
  int fail = 0;

  std::default_random_engine generator;
  std::uniform_real_distribution<double> bY(-40, 40);
  std::uniform_real_distribution<double> bX(-10, 10);

  // Generate uniform pions

  std::uniform_real_distribution<double> PX(-4, 4);
  std::uniform_real_distribution<double> PY(-4, 4);
  std::uniform_real_distribution<double> PZ(0.0, 4);

  std::uniform_real_distribution<double> P(0.05, 4);
  std::uniform_real_distribution<double> THETA(0, 1.57079632679);
  std::uniform_real_distribution<double> PHI(0.0, 6.28318530718);

  // Loop
  // while(inFile>>pdg >> mass >> E >> px >> py >> pz >> p) {
  for (int i_pion = 0; i_pion < pionstates_; i_pion++) {
    p = P(generator);
    double theta = THETA(generator);
    double phi = PHI(generator);
    // px = PX(generator);
    // py = PY(generator);
    // pz = PZ(generator);
    // p = sqrt(px*px + py*py + pz*pz);

    px = p * cos(theta);
    py = p * sin(theta) * cos(phi);
    pz = p * sin(theta) * sin(phi);

    total += 1;
    std::vector<std::vector<Acts::detail::Step>> propagationSteps;

    // randomize beamspot
    // global Y
    double by = bY(generator);

    // global X
    double bx = bX(generator);

    // Already rotated in tracking frame
    gen_pos(0) = 0.;
    gen_pos(1) = bx;
    gen_pos(2) = by;

    // Skip particles that are not propagated in the forward direction from the
    // target.
    if (px < 0) {
      pzCut++;
      continue;
    }

    // Skip particles with a momentum too low to be reconstructed

    if (p < 0.05) {
      pCut++;
      continue;
    }

    // std::cout<<pdg <<" " << mass <<" " << E <<" " << px <<" " <<py<<" "
    // <<pz<<" " <<p<<std::endl;

    // Transform to MeV because that's what TrackUtils assumes
    gen_mom(0) = px / Acts::UnitConstants::MeV;
    gen_mom(1) = py / Acts::UnitConstants::MeV;
    gen_mom(2) = pz / Acts::UnitConstants::MeV;
    
    Acts::ActsScalar q = -1 * Acts::UnitConstants::e;

    if (pdg == 211 || pdg == 2212) q = +1 * Acts::UnitConstants::e;

    Acts::FreeVector part_free =
        tracking::sim::utils::toFreeParameters(gen_pos, gen_mom, q);

    // perigee on the track
    std::shared_ptr<const Acts::PerigeeSurface> gen_surface =
        Acts::Surface::makeShared<Acts::PerigeeSurface>(Acts::Vector3(
            part_free[Acts::eFreePos0], part_free[Acts::eFreePos1],
            part_free[Acts::eFreePos2]));

    // std::cout<<"gen_free"<<std::endl;
    // std::cout<<part_free<<std::endl;

    // Transform the free parameters to the bound parameters
    auto bound_params = Acts::detail::transformFreeToBoundParameters(
                            part_free, *gen_surface, gctx_)
                            .value();

    using RecordedMaterial = Acts::MaterialInteractor::result_type;
    using RecordedMaterialTrack =
        std::pair<std::pair<Acts::Vector3, Acts::Vector3>, RecordedMaterial>;
    using PropagationOutput =
        std::pair<std::vector<Acts::detail::Step>, RecordedMaterial>;

    PropagationOutput pOutput;
    const auto eNLogger = Acts::getDefaultLogger("eN", Acts::Logging::INFO);

    Acts::PropagatorOptions<ActionList, AbortList> propagator_options(
        gctx_, bctx_, Acts::LoggerWrapper{*eNLogger});
    propagator_options.pathLimit = std::numeric_limits<double>::max();
    propagator_options.loopProtection = false;
    auto& mInteractor =
        propagator_options.actionList.get<Acts::MaterialInteractor>();
    mInteractor.multipleScattering = true;
    mInteractor.energyLoss = true;
    mInteractor.recordInteractions = false;

    auto& sLogger =
        propagator_options.actionList.get<Acts::detail::SteppingLogger>();
    sLogger.sterile = false;
    propagator_options.maxStepSize = 5 * Acts::UnitConstants::mm;
    propagator_options.maxSteps = 2000;

    Acts::BoundTrackParameters startParameters(
        gen_surface, std::move(bound_params), std::move(std::nullopt));
    auto result = propagator_->propagate(startParameters, propagator_options);

    if (result.ok()) {
      const auto& resultValue = result.value();
      auto steppingResults =
          resultValue.template get<Acts::detail::SteppingLogger::result_type>();
      pOutput.first = std::move(steppingResults.steps);
      propagationSteps.push_back(std::move(pOutput.first));

      success += 1;
    } else {
      // std::cout<<"PF::ERROR::PROPAGATION RESULTS ARE NOT OK!!"<<std::endl;
      fail += 1;
    }

    // empty
    std::vector<ldmx::Measurement> hits{};
    writer_->WriteSteps(event, propagationSteps, hits, gen_pos, gen_mom);

  }  // loop on inputs
  std::cout << "Total=" << total << " success=" << success << " fail=" << fail
            << " pzCut=" << pzCut << " pCut=" << pCut << std::endl;
}

}  // namespace reco
}  // namespace tracking

DECLARE_PRODUCER_NS(tracking::reco, CKFProcessor)
