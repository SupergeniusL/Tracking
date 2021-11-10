#include "Tracking/Sim/TrackingGeometryMaker.h"


//--- ACTS ---//
#include "Acts/Plugins/TGeo/TGeoPrimitivesHelper.hpp"

//--- DD4Hep ---//
#include "DD4hep/DetElement.h"


//--- C++ StdLib ---//
#include <iostream>

namespace tracking {
namespace sim {
    
TrackingGeometryMaker::TrackingGeometryMaker(const std::string &name,
                                             framework::Process &process)
    : framework::Producer(name, process) {}

TrackingGeometryMaker::~TrackingGeometryMaker() {}

void TrackingGeometryMaker::onProcessStart() {

  detector_ = &detector();
  m_gctx = Acts::GeometryContext();

  
  // Get the world detector element
  dd4hep::DetElement world{detector_->world()};
  std::cout << "World volume name: " << world.name() << std::endl;
  Acts::CuboidVolumeBuilder cvb;
  std::vector<dd4hep::DetElement> subdetectors;

  //Get the ACTS Logger
  auto loggingLevel = Acts::Logging::VERBOSE;
  ACTS_LOCAL_LOGGER(Acts::getDefaultLogger("DD4hepConversion", loggingLevel));
    
  //The subdetectors should be the TaggerTracker and the Recoil Tracker
  
  collectSubDetectors_dd4hep(world,subdetectors);
  std::cout<<"PF::DEBUG::"<<__PRETTY_FUNCTION__<<" size  of subdetectors::"<<subdetectors.size()<<std::endl;

  //loop over the subDetectors to gather all the configurations
  
  std::vector<Acts::CuboidVolumeBuilder::VolumeConfig> volBuilderConfigs;
  for (auto& subDetector : subdetectors) {
      std::cout<<"PF::DEBUG:: Translating DD4Hep sub detector: " << subDetector.name()<<std::endl;
      //create the cuboid volume configurations for the builder
      
      volBuilderConfigs.push_back(volumeBuilder_dd4hep(subDetector,loggingLevel));
  }
  
  //Create the builder
  
  // Test the building 
  
  //std::shared_ptr<Acts::TrackingVolume> trVol =
  //    cvb.buildVolume(m_gctx, volBuilderConfigs[0]);


  Acts::CuboidVolumeBuilder::Config config;
  config.position = {0., 0., 0.};
  config.length = {2000, 2000, 2000};
  config.volumeCfg = volBuilderConfigs;

  cvb.setConfig(config);
  
  
  Acts::TrackingGeometryBuilder::Config tgbCfg;
  tgbCfg.trackingVolumeBuilders.push_back(
      [=](const auto& cxt, const auto& inner, const auto&) {
          return cvb.trackingVolume(cxt, inner, nullptr);
      });
  
  std::cout<<"Tracking Geometry Builder... done"<<std::endl;
  
  Acts::TrackingGeometryBuilder tgb(tgbCfg);
  
  std::unique_ptr<const Acts::TrackingGeometry> tGeometry = 
      tgb.trackingGeometry(m_gctx);

  
  
  //Move this to a function
  
  if (dumpobj_) {
       
       double outputScalor = 1.0;   ///< scale output values
       size_t outputPrecision = 6;  ///< floating point precision
       
       Acts::ObjVisualization3D objVis(outputPrecision, outputScalor);
       
       Acts::ViewConfig containerView = Acts::ViewConfig({220, 220, 220});
       Acts::ViewConfig volumeView = Acts::ViewConfig({220, 220, 0});
       Acts::ViewConfig sensitiveView = Acts::ViewConfig({0, 180, 240});
       Acts::ViewConfig passiveView = Acts::ViewConfig({240, 280, 0});
       Acts::ViewConfig gridView = Acts::ViewConfig({220, 0, 0});
       
       Acts::GeometryView3D::drawTrackingVolume(
           objVis, *(tGeometry->highestTrackingVolume()), m_gctx, containerView,
           volumeView, passiveView, sensitiveView, gridView);
       
   }
   
}

    //A copy is not a good idea. TODO
    Acts::CuboidVolumeBuilder::VolumeConfig TrackingGeometryMaker::volumeBuilder_dd4hep(dd4hep::DetElement& subdetector,Acts::Logging::Level logLevel) {
    
    //ACTS_INFO("Processing detector element:  " << subdetector.name());
    
    // Get the extension, if it exists
    Acts::ActsExtension* subDetExtension = nullptr;
    try {
        subDetExtension = subdetector.extension<Acts::ActsExtension>();
    } catch (std::runtime_error& e) {
    }
    
    //Just a place holder in the case we will make compound detectors. 
    if (subdetector.type() == "compound") {}
    // Now create the Layerbuilders and Volumebuilder
    
    // Define the configuration for the cuboid volume builder object 
    Acts::CuboidVolumeBuilder::Config cvbConfig;
    
    // Get the sensors
    std::vector<dd4hep::DetElement> sensors;
    
    //collect the sensors (just a trick to add the ActsExtension)
    
    collectSensors_dd4hep(subdetector,sensors);
    std::cout<<"PF::DEBUG "<<__PRETTY_FUNCTION__<<" the size of sensors="<<sensors.size()<<std::endl;
    
    std::vector<std::shared_ptr<const Acts::Surface>> surfaces;
    
    //Get all the sensitive surfaces for the tagger Tracker. 
    //For the moment I'm forcing to grep everything.
    
    resolveSensitive(subdetector,surfaces,true);
    std::cout<<"PF::DEBUG "<<__PRETTY_FUNCTION__<<" surfaces size::"<<surfaces.size()<<std::endl;
    
    //Check the surfaces that I created
            
    //for (auto& surface : surfaces) {
        
        //surface->toStream(m_gctx,std::cout);
        //std::cout<<std::endl;
        //surface->surfaceMaterial()->toStream(std::cout);
        //std::cout<<std::endl;
        //std::cout<<surface->surfaceMaterial()->materialSlab(0,0).material().massDensity()<<std::endl;
        //std::cout<<surface->surfaceMaterial()->materialSlab(0,0).material().molarDensity()<<std::endl;
    //}
    
    //Surfaces configs
    std::vector<Acts::CuboidVolumeBuilder::SurfaceConfig> surfaceConfig;

    
    int counter = 0;
    for (auto& sensor : sensors) {
        if (counter >= 1) {
            //break;
        }
        counter++;

        Acts::CuboidVolumeBuilder::SurfaceConfig cfg;
        
        std::cout<<"Getting the transform "<<sensor.name()<<std::endl;
        
        //Get the tranformation from the alignment support. 
        auto transform =
            convertTransform(&(sensor.nominal().worldTransformation()));
        
        //Rotate Z->X, X->Y, Y->Z 
        Acts::Vector3 position = {transform.translation()[2], transform.translation()[0], transform.translation()[1]};
        
        //Rotate the sensors to be orthogonal to X
        double rotationAngle = M_PI * 0.5;
        
        // 0 0 -1
        // 0 1 0
        // 1 0 0
        
        //This rotation is needed to have the plane orthogonal to the X direction. 
        // Rotation of the surfaces
        Acts::Vector3 xPos1(cos(rotationAngle), 0., sin(rotationAngle));
        Acts::Vector3 yPos1(0., 1., 0.);
        Acts::Vector3 zPos1(-sin(rotationAngle), 0., cos(rotationAngle));
        
        Acts::RotationMatrix3 y_rot;
        y_rot.col(0) = xPos1;
        y_rot.col(1) = yPos1;
        y_rot.col(2) = zPos1;

        //Rotate the sensors to put them in the proper orientation in Z
        Acts::Vector3 xPos2(1., 0. ,0.);
        Acts::Vector3 yPos2(0., cos(rotationAngle), sin(rotationAngle));
        Acts::Vector3 zPos2(0., -sin(rotationAngle),cos(rotationAngle));

        Acts::RotationMatrix3 x_rot;
        x_rot.col(0) = xPos2;
        x_rot.col(1) = yPos2;
        x_rot.col(2) = zPos2;
        
        
        cfg.position = position;
        //cfg.rotation = cfg.rotation*transform.rotation();
        cfg.rotation = x_rot*y_rot*transform.rotation();
        
        //Position and rotation of the surface
        std::cout<<cfg.position<<std::endl;
        std::cout<<cfg.rotation<<std::endl;
        
        //Get the bounds -  TODO..FIX hardcoding
        cfg.rBounds  = std::make_shared<const Acts::RectangleBounds>(Acts::RectangleBounds(20.17, 50));
        //cfg.rBounds  = std::make_shared<const Acts::RectangleBounds>(Acts::RectangleBounds(0.000002017, 0.00000050));
        
        // I don't think I need this to be defined.
        cfg.detElementConstructor = nullptr; 
        
        // Thickness
        double thickness = 2*Acts::UnitConstants::cm*sensor.volume().boundingBox().z();
                
        // Material
        
        dd4hep::Material de_mat = sensor.volume().material();
        Acts::Material silicon = Acts::Material::fromMassDensity(de_mat.radLength(),de_mat.intLength(), de_mat.A(), de_mat.Z(), de_mat.density());
        Acts::MaterialSlab silicon_slab(silicon,thickness); 
        
        // Do I need this?!
        cfg.thickness = thickness;
                
        cfg.surMat = std::make_shared<Acts::HomogeneousSurfaceMaterial>(silicon_slab);
        

        surfaceConfig.push_back(cfg);
    }
    
    std::cout<<"Formed " <<surfaceConfig.size()<< " Surface configs"<<std::endl;
    
    //Layer Configurations
    std::vector<Acts::CuboidVolumeBuilder::LayerConfig> layerConfig;
    
    //One layer for surface...  TODO::Is this remotely right?
    for (auto& sCfg : surfaceConfig) {
        Acts::CuboidVolumeBuilder::LayerConfig cfg;
        cfg.surfaceCfg = sCfg;
        layerConfig.push_back(cfg);
        cfg.active = true;
    }
    
    std::cout<<"Formed " <<layerConfig.size()<< " layer configs"<<std::endl;
    
    //Create the volume
    
    // Build the sub-detector volume configuration
    Acts::CuboidVolumeBuilder::VolumeConfig subDetVolumeConfig;
    
    // Get the transform wrt the world
    auto subDet_transform = convertTransform(&(subdetector.nominal().worldTransformation()));
    
    //std::cout<<subDet_transform.translation()<<std::endl;
    //std::cout<<subDet_transform.rotation()<<std::endl;
    subDetVolumeConfig.position = subDet_transform.translation();
    
    double x_length = 2*Acts::UnitConstants::cm*subdetector.volume().boundingBox().x();
    double y_length = 2*Acts::UnitConstants::cm*subdetector.volume().boundingBox().y();
    double z_length = 2*Acts::UnitConstants::cm*subdetector.volume().boundingBox().z();

    std::cout<<"x "<<x_length<<" y "<<y_length<<" z "<<z_length<<std::endl;
    
    subDetVolumeConfig.length = {x_length, y_length, z_length};
    subDetVolumeConfig.layerCfg = layerConfig;
    subDetVolumeConfig.name = subdetector.name();
    
    //Form the Homogeneous material for the tagger volume
    dd4hep::Material subde_mat = subdetector.volume().material();
    Acts::Material subdet_mat = Acts::Material::fromMassDensity(subde_mat.radLength(),
                                                                subde_mat.intLength(), subde_mat.A(), 
                                                                subde_mat.Z(), subde_mat.density()); 
    
    subDetVolumeConfig.volumeMaterial =
        std::make_shared<Acts::HomogeneousVolumeMaterial>(subdet_mat);
    
    
    return subDetVolumeConfig;
    
}

  
  void TrackingGeometryMaker::configure(framework::config::Parameters &parameters) {
    
    // set dumping obj flag (integer for the moment) TODO
    dumpobj_ = parameters.getParameter<int>("dumpobj");
    
    
    
    
  }
  
  void TrackingGeometryMaker::produce(framework::Event &event) {}
    
    
    // **** //
    //std::shared_ptr<const Acts::CylinderVolumeHelper> cuboidVolumeHelper_dd4hep(Logging::Level loggingLevel) {
    //#####################//

    void TrackingGeometryMaker::collectModules_dd4hep(dd4hep::DetElement& detElement,
                                                      std::vector<dd4hep::DetElement> & modules) {
        const dd4hep::DetElement::Children& children = detElement.children();
        std::cout<<__PRETTY_FUNCTION__<<" Collecting from "<<detElement.name()<<std::endl;
    }

    void TrackingGeometryMaker::collectSensors_dd4hep(dd4hep::DetElement& detElement,
                                                      std::vector<dd4hep::DetElement>& sensors) {
        std::cout<<__PRETTY_FUNCTION__<<" Collecting from " <<detElement.name()<<std::endl;
        
        const dd4hep::DetElement::Children& children = detElement.children();
        
        for (auto& child : children) {
            dd4hep::DetElement childDetElement = child.second;
            std::string childType = childDetElement.type();
            
            if (childType != "si_sensor")
                continue;
            
            //Check if there is an Acts extension associated to this detElement. If not, add it.
            
            Acts::ActsExtension* detExtension = nullptr;
            try {
                detExtension = childDetElement.extension<Acts::ActsExtension>();
                std::cout<<detExtension->toString()<<std::endl;
            }
            catch (std::runtime_error& e) {
                //std::cout<<"Caught exception in "<<__PRETTY_FUNCTION__<<std::endl;
                //continue;
            }
            
            //Add the child if the detExtension is the TaggerTracker, the RecoilTracker or the Target(?)
            if ((detExtension!=nullptr)) {
                if (detExtension->hasType("si_sensor","detector")){
                    sensors.push_back(childDetElement);
                }
            }
            else {  //ActsExtension doesn't exist
                std::cout<<__PRETTY_FUNCTION__<<"PF::DEBUG:: Adding the ActsExtension for sensors"<<std::endl;
                detExtension = new Acts::ActsExtension();
                detExtension->addType(childDetElement.name(), "si_sensor");
                childDetElement.addExtension<Acts::ActsExtension>(detExtension);
                //detExtension->addType("axes", "definitions", "XYZ"); //Issue?
                //detExtension->addType("axes", "definitions", "ZYX");
                sensors.push_back(childDetElement);
            }
            
        } // children loop
        
    }// get sensors.

    //Collect the subdetectors and add the ActsExtension to them    
    //I expect to find the TaggerTracker and the RecoilTracker.
void TrackingGeometryMaker::collectSubDetectors_dd4hep(dd4hep::DetElement& detElement,
                                                       std::vector<dd4hep::DetElement>& subdetectors) {
    const dd4hep::DetElement::Children& children = detElement.children();
    
    std::cout<<"Collecting from "<<detElement.name()<<std::endl;
        
    for (auto& child : children) {
        dd4hep::DetElement childDetElement = child.second;
        std::cout<<"Child Name:: "<<childDetElement.name()<<std::endl;
        std::cout<<"Child Type:: "<<childDetElement.type()<<std::endl;
        std::string childName = childDetElement.name();
        
        //Check here if I'm checking the TaggerTracker or the RecoilTracker. Skip the rest.
        if (childName != "TaggerTracker"  &&
            childName != "tagger_tracker" &&
            childName != "recoil_tracker" &&
            childName != "RecoilTracker") {
            continue;
        }
        
        //Check if an Acts extension is attached to the det Element (not necessary)
        Acts::ActsExtension* detExtension = nullptr;
        try {
            detExtension = childDetElement.extension<Acts::ActsExtension>();
            std::cout<<detExtension->toString()<<std::endl;
        }
        catch (std::runtime_error& e) {
            //std::cout<<"Caught exception in "<<__PRETTY_FUNCTION__<<std::endl;
            //continue;
        }
        
        //Add the child if the detExtension is the TaggerTracker, the RecoilTracker or the Target(?)
        if ((detExtension!=nullptr)) {
            if (detExtension->hasType("TaggerTracker","detector") ||  
                detExtension->hasType("RecoilTracker","detector")) {
                subdetectors.push_back(childDetElement);
            }
        }
        else {  //ActsExtension doesn't exist
            std::cout<<"PF::DEBUG:: Adding the ActsExtension to "<< childDetElement.name() << " " <<childDetElement.type() <<std::endl;
            
            detExtension = new Acts::ActsExtension();
            detExtension->addType(childDetElement.name(), "detector");
            childDetElement.addExtension<Acts::ActsExtension>(detExtension);
            detExtension->addType("axes", "definitions", "XYZ"); // no effect in changing this line for the sensors.
            subdetectors.push_back(childDetElement);
        }
        
        //recursive
        collectSubDetectors_dd4hep(childDetElement,subdetectors);
    }//children loop
}


    
    void TrackingGeometryMaker::resolveSensitive(
        const dd4hep::DetElement& detElement,
        std::vector<std::shared_ptr<const Acts::Surface>>& surfaces,bool force) const {
        const dd4hep::DetElement::Children& children = detElement.children();
        std::cout<<"resolving sensitive from "<<detElement.name()<<std::endl;
        
        if (!children.empty()) {
            std::cout<<"Resolving children.."<<std::endl;
            for (auto& child : children) {
                dd4hep::DetElement childDetElement = child.second;
                std::cout<<"childDetElement::"<<childDetElement.name()<< " "<<childDetElement.type()<<std::endl;
                
                
                //Check material
                std::cout<<childDetElement.volume().material().toString()<<std::endl;
                
                if (childDetElement.volume().isSensitive() || force) {
                    std::cout<<"isSensitive.. "<<std::endl;
                    // create the surface
                    surfaces.push_back(createSensitiveSurface(childDetElement));
                }
                resolveSensitive(childDetElement, surfaces,force);
            }
        }
    }//resolve sensitive


    std::shared_ptr<const Acts::Surface>
    TrackingGeometryMaker::createSensitiveSurface(
        const dd4hep::DetElement& detElement) const {
        // access the possible extension of the DetElement
        Acts::ActsExtension* detExtension = nullptr;
        try {
            detExtension = detElement.extension<Acts::ActsExtension>();
        } catch (std::runtime_error& e) {
            std::cout<<"ERROR::"<<__PRETTY_FUNCTION__<<"Could not get Acts::Extension "<<std::endl;
            //ACTS_WARNING("Could not get Acts::Extension");
            return nullptr;
        }
        
        //Axes orientations
        auto detAxis = detExtension->getType("axes", "definitions");

        //Add the material
        dd4hep::Material de_mat = detElement.volume().material();
        //std::cout<<childDetElement.volume().material().toString()<<std::endl;
        //std::cout<<"Silicon density "<<de_mat.density()<<std::endl;
        Acts::Material silicon = Acts::Material::fromMassDensity(de_mat.radLength(),de_mat.intLength(), de_mat.A(), de_mat.Z(), de_mat.density());
        
        //I don't understand the need of the lossy mol_density constructor.
        
        //double kAvogadro = 6.02214076e23;
        //double mol_density = (double) de_mat.density() / ((double) de_mat.A() * kAvogadro);
        //Acts::Material silicon = Acts::Material::fromMolarDensity(de_mat.radLength(),de_mat.intLength(), de_mat.A(), de_mat.Z(), mol_density);
        //std::cout<<"Silicon mole density"<<mol_density<<std::endl;
        
        
        //Get the thickness. The bounding box gives back half of the size in z. I scaled of factor 10 to bring it in mm. The detElement stores in cm units
        double thickness = 2*Acts::UnitConstants::cm*detElement.volume().boundingBox().z();
        
        Acts::MaterialSlab silicon_slab(silicon,thickness); 
        std::shared_ptr<Acts::HomogeneousSurfaceMaterial> homogeneous_mat = std::make_shared<Acts::HomogeneousSurfaceMaterial>(silicon_slab);
        
        // Create the corresponding detector element !- memory leak --!
        //I've checked: cm is the right scalor to use since it picks the right units from the xml. 
        //Using cm here to multiply by 10. 
        Acts::DD4hepDetectorElement* dd4hepDetElement =
            new Acts::DD4hepDetectorElement(detElement, detAxis, Acts::UnitConstants::cm, 
                                            false, homogeneous_mat, nullptr); //is disc is always false.
        
        
        // return the surface
        return dd4hepDetElement->surface().getSharedPtr();
    }

    // I use UnitConstants::cm because the seems like that the dd4hep classes store the informations in cm
    
  Acts::Transform3 TrackingGeometryMaker::convertTransform(
        const TGeoMatrix* tGeoTrans) const {
        // get the placement and orientation in respect to its mother
        const Double_t* rotation = tGeoTrans->GetRotationMatrix();
        const Double_t* translation = tGeoTrans->GetTranslation();

        return Acts::TGeoPrimitivesHelper::makeTransform(
            Acts::Vector3(rotation[0], rotation[3], rotation[6]),
            Acts::Vector3(rotation[1], rotation[4], rotation[7]),
            Acts::Vector3(rotation[2], rotation[5], rotation[8]),
            Acts::Vector3(translation[0] * Acts::UnitConstants::cm,
                           translation[1] * Acts::UnitConstants::cm,
                           translation[2] * Acts::UnitConstants::cm));
    }
    
} // namespace sim
} // namespace tracking

DECLARE_PRODUCER_NS(tracking::sim, TrackingGeometryMaker)
