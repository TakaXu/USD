//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//

// glew needs to be included before any other OpenGL headers.
#include "pxr/imaging/glf/glew.h"

#include "pxr/pxr.h"
#include "pxrUsdMayaGL/batchRenderer.h"
#include "pxrUsdMayaGL/debugCodes.h"
#include "pxrUsdMayaGL/renderParams.h"
#include "pxrUsdMayaGL/sceneDelegate.h"
#include "pxrUsdMayaGL/shapeAdapter.h"
#include "pxrUsdMayaGL/softSelectHelper.h"
#include "pxrUsdMayaGL/userData.h"

#include "px_vp20/utils.h"
#include "px_vp20/utils_legacy.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2i.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/base/gf/vec4d.h"
#include "pxr/base/gf/vec4f.h"
#include "pxr/base/tf/debug.h"
#include "pxr/base/tf/envSetting.h"
#include "pxr/base/tf/getenv.h"
#include "pxr/base/tf/instantiateSingleton.h"
#include "pxr/base/tf/singleton.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/tf/stl.h"
#include "pxr/base/tf/token.h"
#include "pxr/base/vt/types.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/renderIndex.h"
#include "pxr/imaging/hd/rprimCollection.h"
#include "pxr/imaging/hd/task.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/imaging/hdx/intersector.h"
#include "pxr/imaging/hdx/selectionTracker.h"
#include "pxr/imaging/hdx/tokens.h"
#include "pxr/usd/sdf/path.h"

#include <maya/M3dView.h>
#include <maya/MDagPath.h>
#include <maya/MDrawContext.h>
#include <maya/MDrawData.h>
#include <maya/MDrawRequest.h>
#include <maya/MEventMessage.h>
#include <maya/MFileIO.h>
#include <maya/MFrameContext.h>
#include <maya/MGlobal.h>
#include <maya/MMatrix.h>
#include <maya/MSceneMessage.h>
#include <maya/MSelectionContext.h>
#include <maya/MStatus.h>
#include <maya/MString.h>
#include <maya/MTypes.h>
#include <maya/MUserData.h>
#include <maya/MViewport2Renderer.h>

#include <utility>


PXR_NAMESPACE_OPEN_SCOPE


// XXX: Supporting area selections in depth (where an object that is occluded
// by another object in the selection is also selected) currently comes with a
// significant performance penalty if the number of objects grows large, so for
// now we only expose that behavior with an env setting.
TF_DEFINE_ENV_SETTING(PXRMAYAHD_ENABLE_DEPTH_SELECTION,
                      false,
                      "Enables area selection of objects occluded in depth");


TF_DEFINE_PRIVATE_TOKENS(
    _tokens,

    ((BatchRendererRootName, "MayaHdBatchRenderer"))
    ((LegacyViewport, "LegacyViewport"))
    ((Viewport2, "Viewport2"))
    ((MayaEndRenderNotificationName, "UsdMayaEndRenderNotification"))
);


TF_INSTANTIATE_SINGLETON(UsdMayaGLBatchRenderer);

/* static */
void
UsdMayaGLBatchRenderer::Init()
{
    GlfGlewInit();

    GetInstance();
}

/* static */
UsdMayaGLBatchRenderer&
UsdMayaGLBatchRenderer::GetInstance()
{
    return TfSingleton<UsdMayaGLBatchRenderer>::GetInstance();
}

HdRenderIndex*
UsdMayaGLBatchRenderer::GetRenderIndex() const
{
    return _renderIndex.get();
}

SdfPath
UsdMayaGLBatchRenderer::GetDelegatePrefix(const bool isViewport2) const
{
    if (isViewport2) {
        return _viewport2Prefix;
    }

    return _legacyViewportPrefix;
}

bool
UsdMayaGLBatchRenderer::AddShapeAdapter(PxrMayaHdShapeAdapter* shapeAdapter)
{
    if (!TF_VERIFY(shapeAdapter, "Cannot add invalid shape adapter")) {
        return false;
    }

    const bool isViewport2 = shapeAdapter->IsViewport2();

    // Add the shape adapter to the correct bucket based on its renderParams.
    _ShapeAdapterBucketsMap& bucketsMap = isViewport2 ?
        _shapeAdapterBuckets :
        _legacyShapeAdapterBuckets;

    const PxrMayaHdRenderParams renderParams =
        shapeAdapter->GetRenderParams(nullptr, nullptr);
    const size_t renderParamsHash = renderParams.Hash();

    TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
        "Adding shape adapter: %p, isViewport2: %s, renderParamsHash: %zu\n",
        shapeAdapter,
        isViewport2 ? "true" : "false",
        renderParamsHash);

    auto iter = bucketsMap.find(renderParamsHash);
    if (iter == bucketsMap.end()) {
        // If we had no bucket for this particular render param combination
        // then we need to create a new one, but first we make sure that the
        // shape adapter isn't in any other bucket.
        RemoveShapeAdapter(shapeAdapter);

        bucketsMap[renderParamsHash] =
            _ShapeAdapterBucket(renderParams, _ShapeAdapterSet({shapeAdapter}));

        TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
            "    Added to newly created bucket\n");
    } else {
        // Check whether this shape adapter is already in this bucket.
        _ShapeAdapterSet& shapeAdapters = iter->second.second;
        auto setIter = shapeAdapters.find(shapeAdapter);
        if (setIter != shapeAdapters.end()) {
            // If it's already in this bucket, then we're done, and we didn't
            // have to add it.
            TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                "    Not adding, already in correct bucket\n");

            return false;
        }

        // Otherwise, remove it from any other bucket it may be in before
        // adding it to this one.
        RemoveShapeAdapter(shapeAdapter);

        shapeAdapters.insert(shapeAdapter);

        TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
            "    Added to existing bucket\n");
    }

    // Debug dumping of current bucket state.
    if (TfDebug::IsEnabled(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING)) {
        TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
            "    _shapeAdapterBuckets (Viewport 2.0) contents:\n");
        for (const auto& iter : _shapeAdapterBuckets) {
            const size_t bucketHash = iter.first;
            const _ShapeAdapterSet& shapeAdapters = iter.second.second;

            TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                "        renderParamsHash: %zu, bucket size: %zu\n",
                bucketHash,
                shapeAdapters.size());

            for (const auto shapeAdapter : shapeAdapters) {
                TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                    "            shape adapter: %p\n",
                    shapeAdapter);
            }
        }

        TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
            "    _legacyShapeAdapterBuckets (Legacy viewport) contents:\n");
        for (auto& iter : _legacyShapeAdapterBuckets) {
            const size_t bucketHash = iter.first;
            const _ShapeAdapterSet& shapeAdapters = iter.second.second;

            TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                "        renderParamsHash: %zu, bucket size: %zu\n",
                bucketHash,
                shapeAdapters.size());

            for (const auto shapeAdapter : shapeAdapters) {
                TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                    "            shape adapter: %p\n",
                    shapeAdapter);
            }
        }
    }

    // Add the shape adapter to the secondary object handle map.
    _ShapeAdapterHandleMap& handleMap = isViewport2 ?
        _shapeAdapterHandleMap :
        _legacyShapeAdapterHandleMap;
    handleMap[MObjectHandle(shapeAdapter->GetDagPath().node())] = shapeAdapter;

    return true;
}

bool
UsdMayaGLBatchRenderer::RemoveShapeAdapter(PxrMayaHdShapeAdapter* shapeAdapter)
{
    if (!TF_VERIFY(shapeAdapter, "Cannot remove invalid shape adapter")) {
        return false;
    }

    const bool isViewport2 = shapeAdapter->IsViewport2();

    TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
        "Removing shape adapter: %p, isViewport2: %s\n",
        shapeAdapter,
        isViewport2 ? "true" : "false");

    // Remove shape adapter from its bucket in the bucket map.
    _ShapeAdapterBucketsMap& bucketsMap = isViewport2 ?
        _shapeAdapterBuckets :
        _legacyShapeAdapterBuckets;

    size_t numErased = 0u;

    std::vector<size_t> emptyBucketHashes;

    for (auto& iter : bucketsMap) {
        const size_t renderParamsHash = iter.first;
        _ShapeAdapterSet& shapeAdapters = iter.second.second;

        const size_t numBefore = numErased;
        numErased += shapeAdapters.erase(shapeAdapter);

        if (TfDebug::IsEnabled(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING) &&
                numErased > numBefore) {
            TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                "    Removed from bucket with render params hash: %zu\n",
                renderParamsHash);
        }

        if (shapeAdapters.empty()) {
            // This bucket is now empty, so we tag it for removal below.
            emptyBucketHashes.push_back(renderParamsHash);
        }
    }

    // Remove any empty buckets.
    for (const size_t renderParamsHash : emptyBucketHashes) {
        const size_t numErasedBuckets = bucketsMap.erase(renderParamsHash);

        if (TfDebug::IsEnabled(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING) &&
                numErasedBuckets > 0u) {
            TF_DEBUG(PXRUSDMAYAGL_SHAPE_ADAPTER_BUCKETING).Msg(
                "    Removed empty bucket with render params hash: %zu\n",
                renderParamsHash);
        }
    }

    // Remove shape adapter from the secondary DAG path map.
    _ShapeAdapterHandleMap& handleMap = isViewport2 ?
        _shapeAdapterHandleMap :
        _legacyShapeAdapterHandleMap;
    handleMap.erase(MObject(shapeAdapter->GetDagPath().node()));

    return (numErased > 0u);
}

/* static */
void
UsdMayaGLBatchRenderer::Reset()
{
    if (UsdMayaGLBatchRenderer::CurrentlyExists()) {
        MGlobal::displayInfo("Resetting USD Batch Renderer");
        UsdMayaGLBatchRenderer::DeleteInstance();
    }

    UsdMayaGLBatchRenderer::GetInstance();
}

bool
UsdMayaGLBatchRenderer::PopulateCustomCollection(
        const MDagPath& dagPath,
        HdRprimCollection& collection)
{
    // We're drawing "out-of-band", so it doesn't matter if we grab the VP2
    // or the Legacy shape adapter. Prefer VP2, but fall back to Legacy if
    // we can't find the VP2 adapter.
    MObjectHandle objHandle(dagPath.node());
    auto iter = _shapeAdapterHandleMap.find(objHandle);
    if (iter == _shapeAdapterHandleMap.end()) {
        iter = _legacyShapeAdapterHandleMap.find(objHandle);
        if (iter == _legacyShapeAdapterHandleMap.end()) {
            return false;
        }
    }

    // Doesn't really hurt to always add, and ensures that the collection is
    // tracked properly.
    HdChangeTracker& changeTracker = _renderIndex->GetChangeTracker();
    changeTracker.AddCollection(collection.GetName());

    // Only update the collection and mark it dirty if the root paths have
    // actually changed. This greatly affects performance.
    PxrMayaHdShapeAdapter* adapter = iter->second;
    const SdfPathVector& roots = adapter->GetRprimCollection().GetRootPaths();
    if (collection.GetRootPaths() != roots) {
        collection.SetRootPaths(roots);
        collection.SetRenderTags(adapter->GetRprimCollection().GetRenderTags());
        changeTracker.MarkCollectionDirty(collection.GetName());
    }

    return true;
}

// Since we're using a static singleton UsdMayaGLBatchRenderer object, we need
// to make sure that we reset its state when switching to a new Maya scene or
// when opening a different scene.
static
void
_OnMayaNewOrOpenSceneCallback(void* clientData)
{
    if (MFileIO::isImportingFile() || MFileIO::isReferencingFile()) {
        return;
    }

    UsdMayaGLBatchRenderer::Reset();
}

// For Viewport 2.0, we listen for a notification from Maya's rendering
// pipeline that all render passes have completed and then we do some cleanup.
/* static */
void
UsdMayaGLBatchRenderer::_OnMayaEndRenderCallback(
        MHWRender::MDrawContext& context,
        void* clientData)
{
    if (UsdMayaGLBatchRenderer::CurrentlyExists()) {
        UsdMayaGLBatchRenderer::GetInstance()._MayaRenderDidEnd(&context);
    }
}

/* static */
void
UsdMayaGLBatchRenderer::_OnSoftSelectOptionsChangedCallback(void* clientData)
{
    auto batchRenderer = static_cast<UsdMayaGLBatchRenderer*>(clientData);
    int commandResult;
    // -sse == -softSelectEnabled
    MGlobal::executeCommand("softSelect -q -sse", commandResult);
    if (!commandResult) {
        batchRenderer->_objectSoftSelectEnabled = false;
        return;
    }
    // -ssf == -softSelectFalloff
    MGlobal::executeCommand("softSelect -q -ssf", commandResult);
    // fallbackMode 3 == object mode
    batchRenderer->_objectSoftSelectEnabled = (commandResult == 3);
}


/* static */
bool
UsdMayaGLBatchRenderer::_GetViewFromDrawContext(
        const MHWRender::MDrawContext& context,
        M3dView* view)
{
    MString modelPanel;
    if (context.renderingDestination(modelPanel) ==
            MHWRender::MFrameContext::k3dViewport) {
        if (M3dView::getM3dViewFromModelPanel(modelPanel, *view)) {
            return true;
        }
    }

    return false;
}

UsdMayaGLBatchRenderer::UsdMayaGLBatchRenderer() :
        _lastRenderFrameStamp(0u),
        _lastSelectionFrameStamp(0u),
        _legacyRenderPending(false),
        _legacySelectionPending(false),
        _objectSoftSelectEnabled(false),
        _softSelectOptionsCallbackId(0)
{
    _viewport2UsesLegacySelection = TfGetenvBool("MAYA_VP2_USE_VP1_SELECTION",
                                                 false);

    _rootId = SdfPath::AbsoluteRootPath().AppendChild(
        _tokens->BatchRendererRootName);
    _legacyViewportPrefix = _rootId.AppendChild(_tokens->LegacyViewport);
    _viewport2Prefix = _rootId.AppendChild(_tokens->Viewport2);

    _renderIndex.reset(HdRenderIndex::New(&_renderDelegate));
    if (!TF_VERIFY(_renderIndex)) {
        return;
    }

    _taskDelegate.reset(
        new PxrMayaHdSceneDelegate(_renderIndex.get(), _rootId));

    const TfTokenVector renderTags({HdTokens->geometry, HdTokens->proxy});

    _legacyViewportRprimCollection.SetName(TfToken(
        TfStringPrintf("%s_%s",
                       _tokens->BatchRendererRootName.GetText(),
                       _tokens->LegacyViewport.GetText())));
    _legacyViewportRprimCollection.SetReprName(HdTokens->refined);
    _legacyViewportRprimCollection.SetRootPath(_legacyViewportPrefix);
    _legacyViewportRprimCollection.SetRenderTags(renderTags);
    _renderIndex->GetChangeTracker().AddCollection(
        _legacyViewportRprimCollection.GetName());

    _viewport2RprimCollection.SetName(TfToken(
        TfStringPrintf("%s_%s",
                       _tokens->BatchRendererRootName.GetText(),
                       _tokens->Viewport2.GetText())));
    _viewport2RprimCollection.SetReprName(HdTokens->refined);
    _viewport2RprimCollection.SetRootPath(_viewport2Prefix);
    _viewport2RprimCollection.SetRenderTags(renderTags);
    _renderIndex->GetChangeTracker().AddCollection(
        _viewport2RprimCollection.GetName());

    _intersector.reset(new HdxIntersector(_renderIndex.get()));
    _selectionTracker.reset(new HdxSelectionTracker());

    // The batch renderer needs to be reset when changing scenes (either by
    // switching to a new empty scene or by opening a different scene). We
    // listen for these two messages and *not* for kSceneUpdate messages since
    // those are also emitted after a SaveAs operation, in which case we
    // actually do not want to reset the batch renderer. We listen for
    // kBeforeFileRead messages because those fire at the right time (after any
    // existing scene has been closed but before the new scene has been opened),
    // but they are also emitted when a file is imported or referenced, so we
    // must be sure *not* to reset the batch renderer in those cases.
    static MCallbackId afterNewCallbackId = 0;
    if (afterNewCallbackId == 0) {
        afterNewCallbackId =
            MSceneMessage::addCallback(MSceneMessage::kAfterNew,
                                       _OnMayaNewOrOpenSceneCallback);
    }

    static MCallbackId beforeFileReadCallbackId = 0;
    if (beforeFileReadCallbackId == 0) {
        beforeFileReadCallbackId =
            MSceneMessage::addCallback(MSceneMessage::kBeforeFileRead,
                                       _OnMayaNewOrOpenSceneCallback);
    }

    MHWRender::MRenderer* renderer = MHWRender::MRenderer::theRenderer();
    if (!renderer) {
        MGlobal::displayError("Viewport 2.0 renderer not initialized.");
    } else {
        // Note that we do not ever remove this notification handler. Maya
        // ensures that only one handler will be registered for a given name
        // and semantic location.
        renderer->addNotification(
            UsdMayaGLBatchRenderer::_OnMayaEndRenderCallback,
            _tokens->MayaEndRenderNotificationName.GetText(),
            MHWRender::MPassContext::kEndRenderSemantic,
            nullptr);
    }

    // We call _OnSoftSelectOptionsChangedCallback manually once, to initalize
    // _objectSoftSelectEnabled; because of this, it's setup is slightly
    // different - since we're calling from within the constructor, we don't
    // use CurrentlyExists()/GetInstance(), but instead pass this as clientData;
    // this also means we should clean up the callback in the destructor.
    _OnSoftSelectOptionsChangedCallback(this);
    _softSelectOptionsCallbackId =
        MEventMessage::addEventCallback(
                "softSelectOptionsChanged",
                _OnSoftSelectOptionsChangedCallback,
                this);
}

/* virtual */
UsdMayaGLBatchRenderer::~UsdMayaGLBatchRenderer()
{
    _selectionTracker.reset();
    _intersector.reset();
    _taskDelegate.reset();

    // We remove the softSelectOptionsChanged callback because it's passed
    // a this pointer, while others aren't.  We do that, instead of just
    // using CurrentlyExists()/GetInstance() because we call it within the
    // constructor
    MMessage::removeCallback(_softSelectOptionsCallbackId);
}

const UsdMayaGLSoftSelectHelper&
UsdMayaGLBatchRenderer::GetSoftSelectHelper()
{
    _softSelectHelper.Populate();
    return _softSelectHelper;
}

static
void
_GetWorldToViewMatrix(M3dView& view, GfMatrix4d* worldToViewMatrix)
{
    // Legacy viewport implementation.

    if (!worldToViewMatrix) {
        return;
    }

    // Note that we use GfMatrix4d's GetInverse() method to get the
    // world-to-view matrix from the camera matrix and NOT MMatrix's
    // inverse(). The latter was introducing very small bits of floating
    // point error that would sometimes result in the positions of lights
    // being computed downstream as having w coordinate values that were
    // very close to but not exactly 1.0 or 0.0. When drawn, the light
    // would then flip between being a directional light (w = 0.0) and a
    // non-directional light (w = 1.0).
    MDagPath cameraDagPath;
    view.getCamera(cameraDagPath);
    const GfMatrix4d cameraMatrix(cameraDagPath.inclusiveMatrix().matrix);
    *worldToViewMatrix = cameraMatrix.GetInverse();
}

static
void
_GetViewport(M3dView& view, GfVec4d* viewport)
{
    // Legacy viewport implementation.

    if (!viewport) {
        return;
    }

    unsigned int viewX, viewY, viewWidth, viewHeight;
    view.viewport(viewX, viewY, viewWidth, viewHeight);
    *viewport = GfVec4d(viewX, viewY, viewWidth, viewHeight);
}

static
void
_GetWorldToViewMatrix(
        const MHWRender::MDrawContext& context,
        GfMatrix4d* worldToViewMatrix)
{
    // Viewport 2.0 implementation.

    if (!worldToViewMatrix) {
        return;
    }

    MStatus status;
    const MMatrix viewMat =
        context.getMatrix(MHWRender::MFrameContext::kViewMtx, &status);
    *worldToViewMatrix = GfMatrix4d(viewMat.matrix);
}

static
void
_GetViewport(const MHWRender::MDrawContext& context, GfVec4d* viewport)
{
    // Viewport 2.0 implementation.

    if (!viewport) {
        return;
    }

    int viewX, viewY, viewWidth, viewHeight;
    context.getViewportDimensions(viewX, viewY, viewWidth, viewHeight);
    *viewport = GfVec4d(viewX, viewY, viewWidth, viewHeight);
}

void
UsdMayaGLBatchRenderer::Draw(const MDrawRequest& request, M3dView& view)
{
    // Legacy viewport implementation.

    MDrawData drawData = request.drawData();

    const PxrMayaHdUserData* hdUserData =
        static_cast<const PxrMayaHdUserData*>(drawData.geometry());
    if (!hdUserData) {
        return;
    }

    MMatrix projectionMat;
    view.projectionMatrix(projectionMat);
    const GfMatrix4d projectionMatrix(projectionMat.matrix);

    if (hdUserData->boundingBox) {
        MMatrix modelViewMat;
        view.modelViewMatrix(modelViewMat);

        // For the legacy viewport, apply a framebuffer gamma correction when
        // drawing bounding boxes, just like we do when drawing geometry via
        // Hydra.
        glEnable(GL_FRAMEBUFFER_SRGB_EXT);

        px_vp20Utils::RenderBoundingBox(*(hdUserData->boundingBox),
                                        *(hdUserData->wireframeColor),
                                        modelViewMat,
                                        projectionMat);

        glDisable(GL_FRAMEBUFFER_SRGB_EXT);
    }

    if (hdUserData->drawShape && _UpdateLegacyRenderPending(false)) {
        GfMatrix4d worldToViewMatrix;
        _GetWorldToViewMatrix(view, &worldToViewMatrix);

        GfVec4d viewport;
        _GetViewport(view, &viewport);

        _RenderBatches(
                /* vp2Context */ nullptr,
                &view,
                worldToViewMatrix,
                projectionMatrix,
                viewport);
    }

    // Clean up the user data.
    delete hdUserData;
}

void
UsdMayaGLBatchRenderer::Draw(
        const MHWRender::MDrawContext& context,
        const MUserData* userData)
{
    // Viewport 2.0 implementation.

    const MHWRender::MRenderer* theRenderer =
        MHWRender::MRenderer::theRenderer();
    if (!theRenderer || !theRenderer->drawAPIIsOpenGL()) {
        return;
    }

    const PxrMayaHdUserData* hdUserData =
        dynamic_cast<const PxrMayaHdUserData*>(userData);
    if (!hdUserData) {
        return;
    }

    MStatus status;

    const MMatrix projectionMat =
        context.getMatrix(MHWRender::MFrameContext::kProjectionMtx, &status);
    const GfMatrix4d projectionMatrix(projectionMat.matrix);

    if (hdUserData->boundingBox) {
        const MMatrix worldViewMat =
            context.getMatrix(MHWRender::MFrameContext::kWorldViewMtx, &status);

        px_vp20Utils::RenderBoundingBox(*(hdUserData->boundingBox),
                                        *(hdUserData->wireframeColor),
                                        worldViewMat,
                                        projectionMat);
    }

    const MHWRender::MPassContext& passContext = context.getPassContext();
    const MString& passId = passContext.passIdentifier();

    const auto inserted = _drawnMayaRenderPasses.insert(passId.asChar());
    if (!inserted.second) {
        // We've already done a Hydra draw for this Maya render pass, so we
        // don't do another one.
        return;
    }

    const MUint64 frameStamp = context.getFrameStamp();

    if (hdUserData->drawShape && _UpdateRenderFrameStamp(frameStamp)) {
        GfMatrix4d worldToViewMatrix;
        _GetWorldToViewMatrix(context, &worldToViewMatrix);

        GfVec4d viewport;
        _GetViewport(context, &viewport);

        M3dView view;
        const bool hasView = _GetViewFromDrawContext(context, &view);

        _RenderBatches(
                &context,
                hasView ? &view : nullptr,
                worldToViewMatrix,
                projectionMatrix,
                viewport);
    }
}

void UsdMayaGLBatchRenderer::DrawCustomCollection(
        const HdRprimCollection& collection,
        const GfMatrix4d& viewMatrix,
        const GfMatrix4d& projectionMatrix,
        const GfVec4d& viewport,
        const PxrMayaHdRenderParams& params)
{
    // Custom collection implementation.

    PxrMayaHdRenderParams paramsCopy = params;
    paramsCopy.customBucketName = collection.GetName();
    std::vector<_RenderItem> items = {
        {paramsCopy, HdRprimCollectionVector({collection})}
    };

    // Currently, we're just using the existing lighting settings.
    _Render(viewMatrix, projectionMatrix, viewport, items);
}

const HdxIntersector::HitSet*
UsdMayaGLBatchRenderer::TestIntersection(
        const PxrMayaHdShapeAdapter* shapeAdapter,
        M3dView& view,
        const bool singleSelection)
{
    // Legacy viewport implementation.
    //
    // HOWEVER... we may actually be performing a selection for Viewport 2.0 if
    // the MAYA_VP2_USE_VP1_SELECTION environment variable is set. If the
    // view's renderer is Viewport 2.0 AND it is using the legacy
    // viewport-based selection method, we compute the selection against the
    // Viewport 2.0 shape adapter buckets rather than the legacy buckets, since
    // we want to compute selection against what's actually being rendered.

    bool useViewport2Buckets = false;
    SdfPath shapeAdapterDelegateId = shapeAdapter->GetDelegateID();

    MStatus status;
    const M3dView::RendererName rendererName = view.getRendererName(&status);
    if (status == MS::kSuccess &&
            rendererName == M3dView::kViewport2Renderer &&
            _viewport2UsesLegacySelection) {
        useViewport2Buckets = true;

        // We also have to "re-write" the shape adapter's delegateId path.
        // Since we're looking for intersections with Viewport 2.0 delegates,
        // we need to look for selection results using a Viewport 2.0-prefixed
        // path. Note that this assumes that the rest of the path after the
        // prefix is identical between the two viewport renderers.
        shapeAdapterDelegateId =
            shapeAdapterDelegateId.ReplacePrefix(_legacyViewportPrefix,
                                                 _viewport2Prefix);
    }

    _ShapeAdapterBucketsMap& bucketsMap = useViewport2Buckets ?
        _shapeAdapterBuckets :
        _legacyShapeAdapterBuckets;

    // Guard against the user clicking in the viewer before the renderer is
    // setup, or with no shape adapters registered.
    if (!_renderIndex || bucketsMap.empty()) {
        _selectResults.clear();
        return nullptr;
    }

    GfMatrix4d viewMatrix;
    GfMatrix4d projectionMatrix;
    px_LegacyViewportUtils::GetViewSelectionMatrices(view,
                                                     &viewMatrix,
                                                     &projectionMatrix);

    // In the legacy viewport, selection occurs in the local space of SOME
    // object, but we need the view matrix in world space to correctly consider
    // all nodes. Applying localToWorldSpace removes the local space we happen
    // to be in.
    const GfMatrix4d localToWorldSpace(shapeAdapter->GetRootXform().GetInverse());
    viewMatrix = localToWorldSpace * viewMatrix;

    if (_UpdateLegacySelectionPending(false)) {
        _ComputeSelection(bucketsMap,
                          &view,
                          viewMatrix,
                          projectionMatrix,
                          singleSelection);
    }

    const HdxIntersector::HitSet* const hitSet =
        TfMapLookupPtr(_selectResults, shapeAdapterDelegateId);
    if (!hitSet || hitSet->empty()) {
        if (_selectResults.empty()) {
            // If nothing was selected previously AND nothing is selected now,
            // Maya does not refresh the viewport. This would be fine, except
            // that we need to make sure we're ready to respond to another
            // selection. Maya may be calling select() on many shapes in
            // series, so we cannot mark a legacy selection pending here or we
            // will end up re-computing the selection on every call. Instead we
            // simply schedule a refresh of the viewport, at the end of which
            // the end render callback will be invoked and we'll mark a legacy
            // selection pending then.
            // This is not an issue with Viewport 2.0, since in that case we
            // have the draw context's frame stamp to uniquely identify the
            // selection operation.
            view.scheduleRefresh();
        }

        return nullptr;
    }

    TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg("FOUND %zu HIT(s)\n", hitSet->size());
    if (TfDebug::IsEnabled(PXRUSDMAYAGL_QUEUE_INFO)) {
        for (const HdxIntersector::Hit& hit : *hitSet) {
            TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
                "    HIT:\n"
                "        delegateId: %s\n"
                "        objectId  : %s\n"
                "        ndcDepth  : %f\n",
                hit.delegateId.GetText(),
                hit.objectId.GetText(),
                hit.ndcDepth);
        }
    }

    return hitSet;
}

const HdxIntersector::HitSet*
UsdMayaGLBatchRenderer::TestIntersection(
        const PxrMayaHdShapeAdapter* shapeAdapter,
        const MHWRender::MSelectionInfo& selectInfo,
        const MHWRender::MDrawContext& context,
        const bool singleSelection)
{
    // Viewport 2.0 implementation.

    // Guard against the user clicking in the viewer before the renderer is
    // setup, or with no shape adapters registered.
    if (!_renderIndex || _shapeAdapterBuckets.empty()) {
        _selectResults.clear();
        return nullptr;
    }

    GfMatrix4d viewMatrix;
    GfMatrix4d projectionMatrix;
    if (!px_vp20Utils::GetSelectionMatrices(selectInfo,
                                            context,
                                            viewMatrix,
                                            projectionMatrix)) {
        return nullptr;
    }

    M3dView view;
    const bool hasView = _GetViewFromDrawContext(context, &view);

    if (_UpdateSelectionFrameStamp(context.getFrameStamp())) {
        _ComputeSelection(_shapeAdapterBuckets,
                          hasView ? &view : nullptr,
                          viewMatrix,
                          projectionMatrix,
                          singleSelection);
    }

    const HdxIntersector::HitSet* const hitSet =
        TfMapLookupPtr(_selectResults, shapeAdapter->GetDelegateID());
    if (!hitSet || hitSet->empty()) {
        return nullptr;
    }

    TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg("FOUND %zu HIT(s)\n", hitSet->size());
    if (TfDebug::IsEnabled(PXRUSDMAYAGL_QUEUE_INFO)) {
        for (const HdxIntersector::Hit& hit : *hitSet) {
            TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
                "    HIT:\n"
                "        delegateId: %s\n"
                "        objectId  : %s\n"
                "        ndcDepth  : %f\n",
                hit.delegateId.GetText(),
                hit.objectId.GetText(),
                hit.ndcDepth);
        }
    }

    return hitSet;
}

bool
UsdMayaGLBatchRenderer::TestIntersectionCustomCollection(
        const HdRprimCollection& collection,
        const GfMatrix4d& viewMatrix,
        const GfMatrix4d& projectionMatrix,
        GfVec3d* hitPoint)
{
    // Custom collection implementation.
    // Differs from viewport implementations in that it doesn't rely on
    // _ComputeSelection being called first.

    const unsigned int pickResolution = 256u;
    _intersector->SetResolution(GfVec2i(pickResolution, pickResolution));

    HdxIntersector::Params params;
    params.viewMatrix = viewMatrix;
    params.projectionMatrix = projectionMatrix;
    params.alphaThreshold = 0.1f;

    HdxIntersector::HitSet hits;
    if (_TestIntersection(collection, params, true, &hits)) {
        if (hitPoint) {
            *hitPoint = hits.begin()->worldSpaceHitPoint;
        }
        return true;
    }

    return false;
}

/* static */
const HdxIntersector::Hit*
UsdMayaGLBatchRenderer::GetNearestHit(
        const HdxIntersector::HitSet* const hitSet)
{
    if (!hitSet || hitSet->empty()) {
        return nullptr;
    }

    const HdxIntersector::Hit* minHit = &(*hitSet->begin());

    for (const auto& hit : *hitSet) {
        if (hit < *minHit) {
            minHit = &hit;
        }
    }

    return minHit;
}

HdRprimCollectionVector
UsdMayaGLBatchRenderer::_GetIntersectionRprimCollections(
        _ShapeAdapterBucketsMap& bucketsMap,
        const MSelectionList& isolatedObjects,
        const bool useDepthSelection) const
{
    HdRprimCollectionVector rprimCollections;

    if (bucketsMap.empty()) {
        return rprimCollections;
    }

    // Assume the shape adapters are for Viewport 2.0 until we inspect the
    // first one.
    bool isViewport2 = true;

    for (auto& iter : bucketsMap) {
        _ShapeAdapterSet& shapeAdapters = iter.second.second;

        for (PxrMayaHdShapeAdapter* shapeAdapter : shapeAdapters) {
            shapeAdapter->UpdateVisibility(isolatedObjects);

            isViewport2 = shapeAdapter->IsViewport2();

            if (!useDepthSelection) {
                // If we don't care about selecting in depth, only update
                // visibility for the shape adapters. We'll use the full
                // viewport renderer collection for selection instead of the
                // individual shape adapter collections.
                continue;
            }

            rprimCollections.push_back(shapeAdapter->GetRprimCollection());
        }
    }

    if (!useDepthSelection) {
        if (isViewport2) {
            rprimCollections.push_back(_viewport2RprimCollection);
        } else {
            rprimCollections.push_back(_legacyViewportRprimCollection);
        }
    }

    return rprimCollections;
}

bool
UsdMayaGLBatchRenderer::_TestIntersection(
    const HdRprimCollection& rprimCollection,
    HdxIntersector::Params queryParams,
    const bool singleSelection,
    HdxIntersector::HitSet* outHitSet)
{
    queryParams.renderTags = rprimCollection.GetRenderTags();

    HdxIntersector::Result result;

    glPushAttrib(GL_VIEWPORT_BIT |
                 GL_ENABLE_BIT |
                 GL_COLOR_BUFFER_BIT |
                 GL_DEPTH_BUFFER_BIT |
                 GL_STENCIL_BUFFER_BIT |
                 GL_TEXTURE_BIT |
                 GL_POLYGON_BIT);
    const bool r = _intersector->Query(queryParams,
                                       rprimCollection,
                                       &_hdEngine,
                                       &result);
    glPopAttrib();
    if (!r) {
        return false;
    }

    if (singleSelection) {
        HdxIntersector::Hit hit;
        if (!result.ResolveNearest(&hit)) {
            return false;
        }

        outHitSet->insert(hit);
    }
    else if (!result.ResolveUnique(outHitSet)) {
        return false;
    }

    return true;
}

void
UsdMayaGLBatchRenderer::_ComputeSelection(
        _ShapeAdapterBucketsMap& bucketsMap,
        const M3dView* view3d,
        const GfMatrix4d& viewMatrix,
        const GfMatrix4d& projectionMatrix,
        const bool singleSelection)
{
    // Figure out Maya's isolate for this viewport.
    MSelectionList isolatedObjects;
#if MAYA_API_VERSION >= 201700
    if (view3d && view3d->viewIsFiltered()) {
        view3d->filteredObjectList(isolatedObjects);
    }
#endif

    // If the enable depth selection env setting has not been turned on, then
    // we can optimize area/marquee selections by handling collections
    // similarly to a single selection, where we test intersections against the
    // single, viewport renderer-based collection.
    const bool useDepthSelection =
        (!singleSelection && TfGetEnvSetting(PXRMAYAHD_ENABLE_DEPTH_SELECTION));

    const HdRprimCollectionVector rprimCollections =
        _GetIntersectionRprimCollections(
            bucketsMap, isolatedObjects, useDepthSelection);

    TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
        "____________ SELECTION STAGE START ______________ "
        "(singleSelection = %s, %zu collection(s))\n",
        singleSelection ? "true" : "false",
        rprimCollections.size());

    // We may miss very small objects with this setting, but it's faster.
    const unsigned int pickResolution = 256u;

    _intersector->SetResolution(GfVec2i(pickResolution, pickResolution));

    HdxIntersector::Params qparams;
    qparams.viewMatrix = viewMatrix;
    qparams.projectionMatrix = projectionMatrix;
    qparams.alphaThreshold = 0.1f;

    _selectResults.clear();

    for (const HdRprimCollection& rprimCollection : rprimCollections) {
        TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
            "--- Intersection Testing with collection: %s\n",
            rprimCollection.GetName().GetText());

        HdxIntersector::HitSet hits;
        if (!_TestIntersection(
                rprimCollection, qparams, singleSelection, &hits)) {
            continue;
        }

        for (const HdxIntersector::Hit& hit : hits) {
            const auto itIfExists =
                _selectResults.emplace(
                    std::make_pair(hit.delegateId,
                                   HdxIntersector::HitSet({hit})));

            const bool& inserted = itIfExists.second;
            if (!inserted) {
                _selectResults[hit.delegateId].insert(hit);
            }
        }
    }

    // Populate the Hydra selection from the selection results.
    HdSelectionSharedPtr selection(new HdSelection);

    const HdSelection::HighlightMode selectionMode =
        HdSelection::HighlightModeSelect;

    for (const auto& delegateHits : _selectResults) {
        const HdxIntersector::HitSet& hitSet = delegateHits.second;

        for (const HdxIntersector::Hit& hit : hitSet) {
            TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
                "NEW HIT\n"
                "    delegateId   : %s\n"
                "    objectId     : %s\n"
                "    instanceIndex: %d\n"
                "    ndcDepth     : %f\n",
                hit.delegateId.GetText(),
                hit.objectId.GetText(),
                hit.instanceIndex,
                hit.ndcDepth);

            if (!hit.instancerId.IsEmpty()) {
                const VtIntArray instanceIndices(1, hit.instanceIndex);
                selection->AddInstance(selectionMode, hit.objectId, instanceIndices);
            } else {
                selection->AddRprim(selectionMode, hit.objectId);
            }
        }
    }

    _selectionTracker->SetSelection(selection);

    TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
        "^^^^^^^^^^^^ SELECTION STAGE FINISH ^^^^^^^^^^^^^\n");
}

void
UsdMayaGLBatchRenderer::_Render(
        const GfMatrix4d& worldToViewMatrix,
        const GfMatrix4d& projectionMatrix,
        const GfVec4d& viewport,
        const std::vector<_RenderItem>& items)
{
    _taskDelegate->SetCameraState(worldToViewMatrix,
                                  projectionMatrix,
                                  viewport);

    // save the current GL states which hydra may reset to default
    glPushAttrib(GL_LIGHTING_BIT |
                 GL_ENABLE_BIT |
                 GL_POLYGON_BIT |
                 GL_DEPTH_BUFFER_BIT |
                 GL_VIEWPORT_BIT);

    // hydra orients all geometry during topological processing so that
    // front faces have ccw winding. We disable culling because culling
    // is handled by fragment shader discard.
    glFrontFace(GL_CCW); // < State is pushed via GL_POLYGON_BIT
    glDisable(GL_CULL_FACE);

    // note: to get benefit of alpha-to-coverage, the target framebuffer
    // has to be a MSAA buffer.
    glDisable(GL_BLEND);
    glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);

    // In all cases, we can should enable gamma correction:
    // - in viewport 1.0, we're expected to do it
    // - in viewport 2.0 without color correction, we're expected to do it
    // - in viewport 2.0 with color correction, the render target ignores this
    //   bit meaning we properly are blending linear colors in the render
    //   target.  The color management pipeline is responsible for the final
    //   correction.
    glEnable(GL_FRAMEBUFFER_SRGB_EXT);

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    // render task setup
    HdTaskSharedPtrVector tasks = _taskDelegate->GetSetupTasks(); // lighting etc

    for (const auto& iter : items) {
        const PxrMayaHdRenderParams& params = iter.first;
        const size_t paramsHash = params.Hash();

        const HdRprimCollectionVector& rprimCollections = iter.second;

        TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
            "*** renderBucket, parameters hash: %zu, bucket size %zu\n",
            paramsHash,
            rprimCollections.size());

        HdTaskSharedPtrVector renderTasks =
            _taskDelegate->GetRenderTasks(paramsHash, params, rprimCollections);
        tasks.insert(tasks.end(), renderTasks.begin(), renderTasks.end());
    }

    VtValue selectionTrackerValue(_selectionTracker);
    _hdEngine.SetTaskContextData(HdxTokens->selectionState,
                                 selectionTrackerValue);

    _hdEngine.Execute(*_renderIndex, tasks);

    glDisable(GL_FRAMEBUFFER_SRGB_EXT);

    glPopAttrib(); // GL_LIGHTING_BIT | GL_ENABLE_BIT | GL_POLYGON_BIT |
                   // GL_DEPTH_BUFFER_BIT | GL_VIEWPORT_BIT
}

void
UsdMayaGLBatchRenderer::_RenderBatches(
        const MHWRender::MDrawContext* vp2Context,
        const M3dView* view3d,
        const GfMatrix4d& worldToViewMatrix,
        const GfMatrix4d& projectionMatrix,
        const GfVec4d& viewport)
{
    _ShapeAdapterBucketsMap& bucketsMap = bool(vp2Context) ?
        _shapeAdapterBuckets :
        _legacyShapeAdapterBuckets;

    if (bucketsMap.empty()) {
        return;
    }

    // Figure out Maya's isolate for this viewport.
    MSelectionList isolatedObjects;
#if MAYA_API_VERSION >= 201700
    if (view3d && view3d->viewIsFiltered()) {
        view3d->filteredObjectList(isolatedObjects);
    }
#endif

    TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
        "____________ RENDER STAGE START ______________ (%zu buckets)\n",
        bucketsMap.size());

    // A new display refresh signifies that the cached selection data is no
    // longer valid.
    _selectResults.clear();

    // We've already populated with all the selection info we need.  We Reset
    // and the first call to GetSoftSelectHelper in the next render pass will
    // re-populate it.
    _softSelectHelper.Reset();

    std::vector<_RenderItem> items;
    for (const auto& iter : bucketsMap) {
        const PxrMayaHdRenderParams& params = iter.second.first;
        const _ShapeAdapterSet& shapeAdapters = iter.second.second;

        HdRprimCollectionVector rprimCollections;
        for (PxrMayaHdShapeAdapter* shapeAdapter : shapeAdapters) {
            shapeAdapter->UpdateVisibility(isolatedObjects);

            rprimCollections.push_back(shapeAdapter->GetRprimCollection());
        }

        items.push_back(std::make_pair(params, std::move(rprimCollections)));
    }

    // Update lighting depending on VP2/Legacy.
    if (vp2Context) {
        _taskDelegate->SetLightingStateFromMayaDrawContext(*vp2Context);
    } else {
        // Maya does not appear to use GL_LIGHT_MODEL_AMBIENT, but it leaves
        // the default value of (0.2, 0.2, 0.2, 1.0) in place. The first time
        // that the viewport is set to use lights in the scene (instead of the
        // default lights or the no/flat lighting modes), the value is reset to
        // (0.0, 0.0, 0.0, 1.0), and it does not get reverted if/when the
        // lighting mode is changed back.
        // Since in the legacy viewport we get the lighting context from
        // OpenGL, we read in GL_LIGHT_MODEL_AMBIENT as the scene ambient. We
        // therefore need to explicitly set GL_LIGHT_MODEL_AMBIENT to the
        // zero/no ambient value before we do, otherwise we would end up using
        // the "incorrect" (i.e. not what Maya itself uses) default value.
        // This is not a problem in Viewport 2.0, since we do not consult
        // OpenGL at all for any of the lighting context state.
        glPushAttrib(GL_LIGHTING_BIT);

        const GfVec4f zeroAmbient(0.0f, 0.0f, 0.0f, 1.0f);
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, zeroAmbient.data());

        _taskDelegate->SetLightingStateFromVP1(worldToViewMatrix,
                                               projectionMatrix);

        glPopAttrib(); // GL_LIGHTING_BIT
    }

    _Render(worldToViewMatrix,
            projectionMatrix,
            viewport,
            items);

    // Viewport 2 may be rendering in multiple passes, and we want to make sure
    // we draw once (and only once) for each of those passes, so we delay
    // swapping the render queue into the select queue until we receive a
    // notification that all rendering has ended.
    // For the legacy viewport, rendering is done in a single pass and we will
    // not receive a notification at the end of rendering, so we do the swap
    // now.
    if (!vp2Context) {
        _MayaRenderDidEnd(nullptr);
    }

    TF_DEBUG(PXRUSDMAYAGL_QUEUE_INFO).Msg(
        "^^^^^^^^^^^^ RENDER STAGE FINISH ^^^^^^^^^^^^^\n");
}

bool
UsdMayaGLBatchRenderer::_UpdateRenderFrameStamp(const MUint64 frameStamp)
{
    if (_lastRenderFrameStamp == frameStamp) {
        return false;
    }

    _lastRenderFrameStamp = frameStamp;

    return true;
}

bool
UsdMayaGLBatchRenderer::_UpdateSelectionFrameStamp(const MUint64 frameStamp)
{
    if (_lastSelectionFrameStamp == frameStamp) {
        return false;
    }

    _lastSelectionFrameStamp = frameStamp;

    return true;
}

bool
UsdMayaGLBatchRenderer::_UpdateLegacyRenderPending(const bool isPending)
{
    if (_legacyRenderPending == isPending) {
        return false;
    }

    _legacyRenderPending = isPending;

    return true;
}

bool
UsdMayaGLBatchRenderer::_UpdateLegacySelectionPending(const bool isPending)
{
    if (_legacySelectionPending == isPending) {
        return false;
    }

    _legacySelectionPending = isPending;

    return true;
}

void
UsdMayaGLBatchRenderer::StartBatchingFrameDiagnostics()
{
    if (!_sharedDiagBatchCtx) {
        _sharedDiagBatchCtx.reset(new PxrUsdMayaDiagnosticBatchContext());
    }
}

void
UsdMayaGLBatchRenderer::_MayaRenderDidEnd(
        const MHWRender::MDrawContext* /* context */)
{
    // Note that we mark a legacy selection as pending regardless of which
    // viewport renderer is active. This is to ensure that selection works
    // correctly in case the MAYA_VP2_USE_VP1_SELECTION environment variable is
    // being used, in which case even though Viewport 2.0 (MPxDrawOverrides)
    // will be doing the drawing, the legacy viewport (MPxSurfaceShapeUIs) will
    // be handling selection.
    _UpdateLegacySelectionPending(true);

    // End any diagnostics batching.
    _sharedDiagBatchCtx.reset();

    _drawnMayaRenderPasses.clear();
}


PXR_NAMESPACE_CLOSE_SCOPE
