#include "stdafx.h"
#include "Alembic.h"
#include "AlembicPolyMsh.h"
#include "AlembicXForm.h"
#include "SceneEnumProc.h"
#include "Utility.h"
#include "AlembicMetadataUtils.h"
#include "AlembicPointsUtils.h"
#include "AlembicIntermediatePolyMesh3DSMax.h"
#include "CommonMeshUtilities.h"



// From the SDK
// How to calculate UV's for face mapped materials.
//static Point3 basic_tva[3] = { 
//	Point3(0.0,0.0,0.0),Point3(1.0,0.0,0.0),Point3(1.0,1.0,0.0)
//};
//static Point3 basic_tvb[3] = { 
//	Point3(1.0,1.0,0.0),Point3(0.0,1.0,0.0),Point3(0.0,0.0,0.0)
//};
//static int nextpt[3] = {1,2,0};
//static int prevpt[3] = {2,0,1};


AlembicPolyMesh::AlembicPolyMesh(SceneNodePtr eNode, AlembicWriteJob * in_Job, Abc::OObject oParent)
: AlembicObject(eNode, in_Job, oParent), customAttributes("Shape User Properties")//TODO...
{
    std::string xformName = EC_MCHAR_to_UTF8( mINode->GetName() );
	std::string meshName = xformName + "Shape";

    AbcG::OPolyMesh mesh(GetOParent(), meshName.c_str(), GetCurrentJob()->GetAnimatedTs());

    mMeshSchema = mesh.getSchema();
}

AlembicPolyMesh::~AlembicPolyMesh()
{
}

Abc::OCompoundProperty AlembicPolyMesh::GetCompound()
{
    return mMeshSchema;
}


void AlembicPolyMesh::SaveMaterialsProperty(bool bFirstFrame, bool bLastFrame)
{
	if(bLastFrame){
		std::vector<std::string> materialNames;
		mergedMeshMaterialsMap& matMap = materialsMerge.groupMatMap;

		for ( mergedMeshMaterialsMap_it it=matMap.begin(); it != matMap.end(); it++)
		{
			meshMaterialsMap& map = it->second;
			for( meshMaterialsMap_it it2 =map.begin(); it2 != map.end(); it2++)
			{
				std::stringstream nameStream;
				int nMaterialId = it2->second.matId+1;
				nameStream<<it2->second.name<<"_"<<nMaterialId;
				materialNames.push_back(nameStream.str());
			}
		}

		if(materialNames.size() > 0){
			if(!mMatNamesProperty.valid())
			{
				mMatNamesProperty = Abc::OStringArrayProperty(mMeshSchema, ".materialnames", mMeshSchema.getMetaData(), GetCurrentJob()->GetAnimatedTs());
			}
			Abc::StringArraySample sample = Abc::StringArraySample(materialNames);
			mMatNamesProperty.set(sample);
		}
	}
}



bool AlembicPolyMesh::Save(double time, bool bLastFrame)
{   
    ESS_PROFILE_FUNC();

	//this call is here to avoid reading pointers that are only valid on a single frame
	mMeshSample.reset();

	const bool bFirstFrame = mNumSamples == 0;

	TimeValue ticks = GetTimeValueFromFrame(time);

	Object *obj = mINode->EvalWorldState(ticks).obj;
	const bool bIsParticleSystem = obj->IsParticleSystem() == 1;

	if(bIsParticleSystem){
		bForever = false;
	}
	else{
		if(mNumSamples == 0){
			bForever = CheckIfObjIsValidForever(obj, ticks);
		}
		else{
			bool bNewForever = CheckIfObjIsValidForever(obj, ticks);
			if(bForever && bNewForever != bForever){
				ESS_LOG_INFO( "bForever has changed" );
			}
		}
	}

    //TODO: should metadata be saved here? or on the transform only
	//SaveMetaData(GetRef().node, this);
    // store the metadata
    //IMetaDataManager mng;
    // mng.GetMetaData(GetRef().node, 0);
    // SaveMetaData(prim.GetParent3DObject().GetRef(),this);

    // check if the mesh is animated (Otherwise, no need to export)
    if(mNumSamples > 0) 
    {
        if(bForever)
        {
			ESS_LOG_INFO( "Node is not animated, not saving topology on subsequent frames." );
            return true;
        }
    }

	IntermediatePolyMesh3DSMax finalMesh;

	if(bIsParticleSystem){

        bool bEnableVelocityExport = true;
		bool bSuccess = getParticleSystemMesh(ticks, obj, mINode, &finalMesh, &materialsMerge, mJob, mNumSamples, bEnableVelocityExport);
		if(!bSuccess){
			ESS_LOG_INFO( "Error. Could not get particle system mesh. Time: "<<time );
			return false;
		}

        velocityCalc.calcVelocities(finalMesh.posVec, finalMesh.mFaceIndicesVec, finalMesh.mVelocitiesVec, GetSecondsFromTimeValue(ticks));
	}
	else
	{
		PolyObject *polyObj = NULL;
		TriObject *triObj = NULL;
		Mesh *triMesh = NULL;
		MNMesh* polyMesh = NULL;

		if (obj->CanConvertToType(Class_ID(POLYOBJ_CLASS_ID, 0)))
		{
			polyObj = reinterpret_cast<PolyObject *>(obj->ConvertToType(ticks, Class_ID(POLYOBJ_CLASS_ID, 0)));
			polyMesh = &polyObj->GetMesh();
		}
		else if (obj->CanConvertToType(Class_ID(TRIOBJ_CLASS_ID, 0)))
		{
			triObj = reinterpret_cast<TriObject *>(obj->ConvertToType(ticks, Class_ID(TRIOBJ_CLASS_ID, 0)));
			triMesh = &triObj->GetMesh();
		}

		// Make sure we have a poly or a tri object
		if (polyMesh == NULL && triMesh == NULL)
		{
			return false;
		}

		//keep the orignal material IDs, since we are not saving out a single nonmerged mesh
		materialsMerge.bPreserveIds = true;

		Matrix3 worldTrans;
		worldTrans.IdentityMatrix();
		finalMesh.Save(mJob->mOptions, triMesh, polyMesh, worldTrans, mINode->GetMtl(), -1, mNumSamples == 0, &materialsMerge);

	   // Note that the TriObject should only be deleted
	   // if the pointer to it is not equal to the object
	   // pointer that called ConvertToType()
	   if (polyObj != NULL && polyObj != obj)
	   {
		   delete polyObj;
		   polyObj = NULL;
	   }

	   if (triObj != NULL && triObj != obj)
	   {
		   delete triObj;
		   triObj = NULL;
	   }
	}

	bool dynamicTopology = static_cast<bool>(GetCurrentJob()->GetOption("exportDynamicTopology"));
	
	// Extend the archive bounding box
	if (mJob){
		Abc::M44d wm;
		ConvertMaxMatrixToAlembicMatrix(mINode->GetObjTMAfterWSM(ticks), wm);

		Abc::Box3d bbox = finalMesh.bbox;

		//ESS_LOG_INFO( "Archive bbox: min("<<bbox.min.x<<", "<<bbox.min.y<<", "<<bbox.min.z<<") max("<<bbox.max.x<<", "<<bbox.max.y<<", "<<bbox.max.z<<")" );

		bbox.min = finalMesh.bbox.min * wm;
		bbox.max = finalMesh.bbox.max * wm;

		//ESS_LOG_INFO( "Archive bbox: min("<<bbox.min.x<<", "<<bbox.min.y<<", "<<bbox.min.z<<") max("<<bbox.max.x<<", "<<bbox.max.y<<", "<<bbox.max.z<<")" );

		mJob->GetArchiveBBox().extendBy(bbox);

		Abc::Box3d box = mJob->GetArchiveBBox();
		//ESS_LOG_INFO( "Archive bbox: min("<<box.min.x<<", "<<box.min.y<<", "<<box.min.z<<") max("<<box.max.x<<", "<<box.max.y<<", "<<box.max.z<<")" );
	}

	mMeshSample.setPositions(Abc::P3fArraySample(finalMesh.posVec));

	mMeshSample.setSelfBounds(finalMesh.bbox);
	mMeshSchema.getChildBoundsProperty().set(finalMesh.bbox);
	
    // abort here if we are just storing points
    if(mJob->GetOption("exportPurePointCache"))
    {
        if(mNumSamples == 0)
        {
            // store a dummy empty topology
			mMeshSample.setFaceCounts(Abc::Int32ArraySample(NULL, 0));
			mMeshSample.setFaceIndices(Abc::Int32ArraySample(NULL, 0));
        }

        mMeshSchema.set(mMeshSample);
        mNumSamples++;
        return true;
    }

	if(mJob->GetOption("validateMeshTopology")){
		mJob->mMeshErrors += validateAlembicMeshTopo(finalMesh.mFaceCountVec, finalMesh.mFaceIndicesVec, EC_MCHAR_to_UTF8(mINode->GetName()));
	}

	Abc::Int32ArraySample faceCountSample(finalMesh.mFaceCountVec);
	Abc::Int32ArraySample faceIndicesSample(finalMesh.mFaceIndicesVec);
	mMeshSample.setFaceCounts(faceCountSample);
	mMeshSample.setFaceIndices(faceIndicesSample);

	if(mJob->GetOption("exportNormals")){
		AbcG::ON3fGeomParam::Sample normalSample;
		normalSample.setScope(AbcG::kFacevaryingScope);
		normalSample.setVals(Abc::N3fArraySample(finalMesh.mIndexedNormals.values));
		normalSample.setIndices(Abc::UInt32ArraySample(finalMesh.mIndexedNormals.indices));
		mMeshSample.setNormals(normalSample);
	}

	AbcG::OV2fGeomParam::Sample uvSample;

	//write out the texture coordinates if necessary
	if(mJob->GetOption("exportUVs"))
	{
       if(correctInvalidUVs(finalMesh.mIndexedUVSet)){
          ESS_LOG_WARNING("Capped out of range uvs on object "<<mINode->GetName()<<", frame = "<<time);
       }
	   saveIndexedUVs( mMeshSchema, mMeshSample, uvSample, mUvParams, mJob->GetAnimatedTs(), mNumSamples, finalMesh.mIndexedUVSet );
	}


	// sweet, now let's have a look at face sets (really only for first sample)
	// for 3DS Max, we are mapping this to the material ids
	if(GetCurrentJob()->GetOption("exportMaterialIds"))
	{
		if(!mMatIdProperty.valid())
		{
			mMatIdProperty = Abc::OUInt32ArrayProperty(mMeshSchema, ".materialids", mMeshSchema.getMetaData(), GetCurrentJob()->GetAnimatedTs());
		}

		Abc::UInt32ArraySample sample = Abc::UInt32ArraySample(finalMesh.mMatIdIndexVec);
		mMatIdProperty.set(sample);

		SaveMaterialsProperty(bFirstFrame, bLastFrame || bForever);

		size_t numMatId = finalMesh.mFaceSetsMap.size();
      bool bExportAllFaceset = GetCurrentJob()->GetOption("partitioningFacesetsOnly") == false;
		// For sample zero, export the material ids as face sets
		if (bFirstFrame && (numMatId > 1 || bExportAllFaceset))
		{
			for ( facesetmap_it it=finalMesh.mFaceSetsMap.begin(); it != finalMesh.mFaceSetsMap.end(); it++)
			{
				std::stringstream nameStream;
				int nMaterialId = it->first+1;
				nameStream<<it->second.name<<"_"<<nMaterialId;

				std::vector<Abc::int32_t>& faceSetVec = it->second.faceIds;

				AbcG::OFaceSet faceSet = mMeshSchema.createFaceSet(nameStream.str());
				AbcG::OFaceSetSchema::Sample faceSetSample(Abc::Int32ArraySample(&faceSetVec.front(), faceSetVec.size()));
				faceSet.getSchema().set(faceSetSample);
			}
		}

	}

   /*if(bFirstFrame || (dynamicTopology))
   {
      // check if we need to export the bindpose (also only for first frame)
	  
      if(GetJob()->GetOption(L"exportBindPose") && prim.GetParent3DObject().GetEnvelopes().GetCount() > 0 && mNumSamples == 0)
      {
         mBindPoseProperty = OV3fArrayProperty(mMeshSchema, ".bindpose", mMeshSchema.getMetaData(), GetJob()->GetAnimatedTs());

         // store the positions of the modeling stack into here
         PolygonMesh bindPoseGeo = prim.GetGeometry(time, siConstructionModeModeling);
         CVector3Array bindPosePos = bindPoseGeo.GetPoints().GetPositionArray();
         mBindPoseVec.resize((size_t)bindPosePos.GetCount());
         for(LONG i=0;i<bindPosePos.GetCount();i++)
         {
            mBindPoseVec[i].x = (float)bindPosePos[i].GetX();
            mBindPoseVec[i].y = (float)bindPosePos[i].GetY();
            mBindPoseVec[i].z = (float)bindPosePos[i].GetZ();
         }

         Abc::V3fArraySample sample;
         if(mBindPoseVec.size() > 0)
            sample = Abc::V3fArraySample(&mBindPoseVec.front(),mBindPoseVec.size());
         mBindPoseProperty.set(sample);
      }
      
   }*/


   // check if we should export the velocities
   // TODO: support velocity property for nonparticle system meshes if possible
	//TODO: add export velocities option
	if(bIsParticleSystem)
	{
		if(finalMesh.posVec.size() != finalMesh.mVelocitiesVec.size()){
			ESS_LOG_INFO("mVelocitiesVec has wrong size.");
		}
		mMeshSample.setVelocities(Abc::V3fArraySample(finalMesh.mVelocitiesVec));
	}


   // save the sample
   mMeshSchema.set(mMeshSample);

   mNumSamples++;


	///////////////////////////////////////////////////////////////////////////////////////////////

    return true;
}
