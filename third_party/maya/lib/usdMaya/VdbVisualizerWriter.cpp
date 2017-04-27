#include "usdMaya/VdbVisualizerWriter.h"
#include "usdMaya/writeUtil.h"
#include "pxr/usd/usdAi/aiVolume.h"
#include "pxr/usd/usdAi/aiNodeAPI.h"
#include "pxr/usd/usdAi/aiShapeAPI.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/stage.h"

#include <maya/MDataHandle.h>

#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {
    const TfToken filename_token("filename");
    const TfToken velocity_grids_token("velocity_grids");
    const TfToken velocity_scale_token("velocity_scale");
    const TfToken velocity_fps_token("velocity_fps");
    const TfToken velocity_shutter_start_token("velocity_shutter_start");
    const TfToken velocity_shutter_end_token("velocity_shutter_end");
    const TfToken bounds_slack_token("bounds_slack");

    UsdAttribute get_attribute(UsdPrim& prim, UsdAiNodeAPI& api, const TfToken& attr_name, const SdfValueTypeName& type) {
        if (prim.HasAttribute(attr_name)) {
            return prim.GetAttribute(attr_name);
        } else {
            return api.CreateUserAttribute(attr_name, type);
        }
    }

    UsdAttribute get_attribute(UsdPrim& prim, const TfToken& attr_name) {
        if (prim.HasAttribute(attr_name)) {
            return prim.GetAttribute(attr_name);
        } else {
            return UsdAttribute();
        }
    }

    bool export_grids(UsdPrim& prim, UsdAiNodeAPI& api, const MFnDependencyNode& node, const char* maya_attr_name, const TfToken& usd_attr_name) {
        const auto grids_string = node.findPlug(maya_attr_name).asString();
        MStringArray grids;
        grids_string.split(' ', grids);
        const auto grids_length = grids.length();
        if (grids_length > 0) {
            VtStringArray grid_names;
            grid_names.reserve(grids_length);
            for (std::remove_const<decltype(grids_length)>::type i = 0; i < grids_length; ++i) {
                grid_names.push_back(grids[i].asChar());
            }
            get_attribute(prim, api, usd_attr_name, SdfValueTypeNames->StringArray)
                .Set(grid_names);
            return true;
        } else {
            return false;
        }
    }
}

VdbVisualizerWriter::VdbVisualizerWriter(const MDagPath & iDag,
                                         const SdfPath& uPath,
                                         bool instanceSource,
                                         usdWriteJobCtx& job) :
    MayaTransformWriter(iDag, uPath, instanceSource, job), has_velocity_grids(false) {
    UsdAiVolume primSchema =
        UsdAiVolume::Define(getUsdStage(), getUsdPath());
    TF_AXIOM(primSchema);
    mUsdPrim = primSchema.GetPrim();
    TF_AXIOM(mUsdPrim);
}

VdbVisualizerWriter::~VdbVisualizerWriter() {
    UsdAiVolume primSchema(mUsdPrim);
    UsdAiNodeAPI nodeApi(primSchema);
    UsdAiShapeAPI shapeApi(primSchema);
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(primSchema.GetStepSizeAttr());
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(shapeApi.GetMatteAttr(), UsdInterpolationTypeHeld);
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(shapeApi.GetReceiveShadowsAttr(), UsdInterpolationTypeHeld);
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(shapeApi.GetSelfShadowsAttr(), UsdInterpolationTypeHeld);
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(nodeApi.GetUserAttribute(filename_token), UsdInterpolationTypeHeld);
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(nodeApi.GetUserAttribute(velocity_scale_token));
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(nodeApi.GetUserAttribute(velocity_fps_token));
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(nodeApi.GetUserAttribute(velocity_shutter_start_token));
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(nodeApi.GetUserAttribute(velocity_shutter_end_token));
    PxrUsdMayaWriteUtil::CleanupAttributeKeys(nodeApi.GetUserAttribute(bounds_slack_token));
}

void VdbVisualizerWriter::write(const UsdTimeCode& usdTime) {
    UsdAiVolume primSchema(mUsdPrim);
    UsdAiNodeAPI nodeApi(primSchema);
    UsdAiShapeAPI shapeApi(primSchema);
    writeTransformAttrs(usdTime, primSchema);

    const MFnDependencyNode volume_node(getDagPath().node());

    // some of the attributes that don't need to be animated has to be exported here
    if (usdTime.IsDefault()) {
        has_velocity_grids = export_grids(mUsdPrim, nodeApi, volume_node, "velocity_grids", velocity_grids_token);
        primSchema.GetDsoAttr().Set(std::string("volume_openvdb"));
    }

    if (usdTime.IsDefault() == getArgs().exportAnimation) {
        return;
    }

    // The node regenerates all kinds of params, so we always need to write these out.
    const auto out_vdb_path = volume_node.findPlug("outVdbPath").asString();
    const auto& bbox_min = volume_node.findPlug("bboxMin").asMDataHandle().asFloat3();
    const auto& bbox_max = volume_node.findPlug("bboxMax").asMDataHandle().asFloat3();
    VtVec3fArray extents(2);
    extents[0] = GfVec3f(bbox_min[0], bbox_min[1], bbox_min[2]);
    extents[1] = GfVec3f(bbox_max[0], bbox_max[1], bbox_max[2]);
    primSchema.GetExtentAttr().Set(extents, usdTime);

    const auto sampling_quality = volume_node.findPlug("samplingQuality").asFloat();
    primSchema.GetStepSizeAttr().Set(volume_node.findPlug("voxelSize").asFloat() / (sampling_quality / 100.0f), usdTime);
    shapeApi.CreateMatteAttr().Set(volume_node.findPlug("matte").asBool(), usdTime);
    shapeApi.CreateReceiveShadowsAttr().Set(volume_node.findPlug("receiveShadows").asBool(), usdTime);
    shapeApi.CreateSelfShadowsAttr().Set(volume_node.findPlug("selfShadows").asBool(), usdTime);
    get_attribute(mUsdPrim, nodeApi, filename_token, SdfValueTypeNames->String)
        .Set(std::string(out_vdb_path.asChar()), usdTime);

    if (has_velocity_grids) {
        get_attribute(mUsdPrim, nodeApi, velocity_scale_token, SdfValueTypeNames->Float)
            .Set(volume_node.findPlug("velocityScale").asFloat(), usdTime);
        get_attribute(mUsdPrim, nodeApi, velocity_fps_token, SdfValueTypeNames->Float)
            .Set(volume_node.findPlug("velocityFps").asFloat(), usdTime);
        get_attribute(mUsdPrim, nodeApi, velocity_shutter_start_token, SdfValueTypeNames->Float)
            .Set(volume_node.findPlug("velocityShutterStart").asFloat(), usdTime);
        get_attribute(mUsdPrim, nodeApi, velocity_shutter_end_token, SdfValueTypeNames->Float)
            .Set(volume_node.findPlug("velocityShutterEnd").asFloat(), usdTime);
    }

    get_attribute(mUsdPrim, nodeApi, bounds_slack_token, SdfValueTypeNames->Float)
        .Set(volume_node.findPlug("boundsSlack").asFloat());
}

PXR_NAMESPACE_CLOSE_SCOPE