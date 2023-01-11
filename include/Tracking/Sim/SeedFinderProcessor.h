#ifndef TRACKING_SIM_SEEDFINDERPROCESSOR_H_
#define TRACKING_SIM_SEEDFINDERPROCESSOR_H_



//---< Framework >---//
#include "Framework/Event.h"
#include "Framework/Configure/Parameters.h"
#include "Framework/EventProcessor.h"

//---< Tracking >---//
#include "Tracking/Sim/LdmxSpacePoint.h"
#include "Tracking/Sim/SeedToTrackParamMaker.h"
#include "Tracking/Sim/TrackingUtils.h"

//---< SimCore >---//
#include "SimCore/Event/SimTrackerHit.h"

//---< STD C++ >---//

#include <iostream>

//---< ACTS >---//
#include "Acts/MagneticField/MagneticFieldContext.hpp"
#include "Acts/Utilities/CalibrationContext.hpp"

#include "Acts/Definitions/Algebra.hpp"

#include "Acts/Seeding/SpacePointGrid.hpp"
#include "Acts/Seeding/Seedfinder.hpp"
#include "Acts/Seeding/SeedFilter.hpp"
#include "Acts/Seeding/Seed.hpp"
#include "Acts/Seeding/BinFinder.hpp"
#include "Acts/Seeding/BinnedSPGroup.hpp"
#include "Acts/Seeding/EstimateTrackParamsFromSeed.hpp"


//--- LDMX ---//
#include "Tracking/Event/Measurement.h"
#include "Tracking/Reco/TrackersTrackingGeometry.h"

#include "TFile.h"
#include "TTree.h"

namespace tracking {
  namespace sim {

  class SeedFinderProcessor : public framework::Producer {

    public:
      /**
       * Constructor.
       *
       * @param name The name of the instance of this object.
       * @param process The process running this producer.
       */
      SeedFinderProcessor(const std::string &name, framework::Process &process);

      /// Destructor
      ~SeedFinderProcessor();

      /**
       *
       */
      void onProcessStart() final override;

      /**
       *
       */
      void onProcessEnd() final override;
       

      /**
       * Configure the processor using the given user specified parameters.
       *
       * @param parameters Set of parameters used to configure this processor.
       */
      void configure(framework::config::Parameters &parameters) final override;

      /**
       * Run the processor and create a collection of results which
       * indicate if a charge particle can be found by the recoil tracker.
       *
       * @param event The event to process.
       */
      void produce(framework::Event &event);
    
   private:
    
    ldmx::Track SeedTracker(const std::vector<ldmx::Measurement>& vmeas,
                            double xOrigin,
                            const Acts::Vector3& perigee_location);

    void LineParabolaToHelix(const Acts::ActsVector<5> parameters, Acts::ActsVector<5>& helix_parameters, Acts::Vector3 ref);
    
        
    /// The tracking geometry
    std::shared_ptr<tracking::reco::TrackersTrackingGeometry> ldmx_tg;
    
    /// The contexts
    Acts::GeometryContext gctx_;
    Acts::MagneticFieldContext bctx_;
    Acts::CalibrationContext cctx_;
    
    
    Acts::SpacePointGridConfig grid_conf_;
    Acts::SeedfinderConfig<ldmx::LdmxSpacePoint> config_;
    Acts::SeedFilterConfig seed_filter_cfg_;
    Acts::Vector3 bField_;
    
    //Acts::Seedfinder::State state_;
    
    std::shared_ptr<Acts::Seedfinder<ldmx::LdmxSpacePoint> > seed_finder_;
    std::shared_ptr<Acts::BinFinder<ldmx::LdmxSpacePoint> >  bottom_bin_finder_;
    std::shared_ptr<Acts::BinFinder<ldmx::LdmxSpacePoint> >  top_bin_finder_;
    
    /* This is a temporary (working) solution to estimate the track parameters out of the seeds
     * Eventually we should move to what is in ACTS (I'm not happy with what they did regarding this part atm)
     */
    
    std::shared_ptr<tracking::sim::SeedToTrackParamMaker> seed_to_track_maker_;
    
    double processing_time_{0.};
    long nevents_{0};
    unsigned int  ntracks_{0};
    /// The name of the output collection of seeds to be stored.
    std::string out_seed_collection_{"SeedTracks"};
    /// The name of the input hits collection to use in finding seeds..
    std::string input_hits_collection_{"TaggerSimHits"};
    /// Location of the perigee for the helix track parameters.
    std::vector<double> perigee_location_{-700.,0.,0};
    /// Minimum cut on the momentum of the seeds.
    double pmin_{0.05};
    /// Maximum cut on the momentum of the seeds.
    double pmax_{8};
    /// Max d0 allowed for the seeds.
    double d0max_{20.};
    /// Min d0 allowed for the seeds.
    double d0min_{20.};
    /// Max z0 allowed for the seeds.
    double z0max_{60.};
    /// List of stragies for seed finding.
    std::vector<std::string> strategies_{};
    
    
    TFile* outputFile_;
    TTree* outputTree_;
    
    std::vector<float> xhit_;
    std::vector<float> yhit_;
    std::vector<float> zhit_;

    std::vector<float> b0_;
    std::vector<float> b1_;
    std::vector<float> b2_;
    std::vector<float> b3_;
    std::vector<float> b4_;

    //Check failures
    long ndoubles_{0};
    long nmissing_{0};
    long nfailpmin_{0};
    long nfailpmax_{0};
    long nfaild0min_{0};
    long nfaild0max_{0};
    long nfailz0max_{0};
    
    
    
    
    
    
  }; // SeedFinderProcessor
  
  
  } // namespace sim
} // namespace tracking

#endif // TRACKING_SIM_SEEDFINDERPROCESSOR_H_