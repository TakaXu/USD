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
#include "pxr/pxr.h"
#include "usdMaya/translatorModelAssembly.h"

#include "usdMaya/primReaderArgs.h"
#include "usdMaya/primReaderContext.h"
#include "usdMaya/primWriterArgs.h"
#include "usdMaya/primWriterContext.h"
#include "usdMaya/referenceAssembly.h"
#include "usdMaya/stageCache.h"
#include "usdMaya/translatorUtil.h"
#include "usdMaya/translatorXformable.h"
#include "usdMaya/util.h"

#include "pxr/base/tf/token.h"
#include "pxr/usd/kind/registry.h"
#include "pxr/usd/sdf/assetPath.h"
#include "pxr/usd/sdf/listOp.h"
#include "pxr/usd/sdf/path.h"
#include "pxr/usd/sdf/reference.h"
#include "pxr/usd/sdf/schema.h"
#include "pxr/usd/sdf/types.h"
#include "pxr/usd/usd/modelAPI.h"
#include "pxr/usd/usd/prim.h"
#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/stageCacheContext.h"
#include "pxr/usd/usd/timeCode.h"
#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usdGeom/xformable.h"
#include "pxr/usd/usdUtils/pipeline.h"

#include <maya/MDagModifier.h>
#include <maya/MFnAssembly.h>
#include <maya/MFnDagNode.h>
#include <maya/MFnData.h>
#include <maya/MFnDependencyNode.h>
#include <maya/MFnTypedAttribute.h>
#include <maya/MGlobal.h>
#include <maya/MObject.h>
#include <maya/MPlug.h>
#include <maya/MString.h>
#include <maya/MFileIO.h>

#include <map>
#include <string>
#include <vector>

// TODO: instead of hardcoding this, switch based on context and
// PIXMAYA_USE_USD_REF_ASSEMBLIES: ie, if the result of a reference or assembly
// operation, do that, but if importing, use the PIXMAYA_USE_USD_REF_ASSEMBLIES
// setting
#define PIXMAYA_FORCE_MAYA_REFERENCES 1

PXR_NAMESPACE_OPEN_SCOPE


TF_DEFINE_PRIVATE_TOKENS(_tokens,
    ((FilePathPlugName, "filePath"))
    ((PrimPathPlugName, "primPath"))
    ((KindPlugName, "kind"))
    ((VariantSetPlugNamePrefix, "usdVariantSet_"))

    ((MayaProxyShapeNameSuffix, "Proxy"))

    // XXX: These should eventually be replaced/removed when the proxy shape
    // node supports all variantSets and not just modelingVariant.
    (variantKey)
    (modelingVariant)
);


/* static */
bool
PxrUsdMayaTranslatorModelAssembly::Create(
        const PxrUsdMayaPrimWriterArgs& args,
        PxrUsdMayaPrimWriterContext* context)
{
    UsdStageRefPtr stage = context->GetUsdStage();
    SdfPath authorPath = context->GetAuthorPath();
    UsdTimeCode usdTime = context->GetTimeCode();

    context->SetExportsGprims(false);
    context->SetExportsReferences(true);
    context->SetPruneChildren(true);

    UsdPrim prim = stage->DefinePrim(authorPath);
    if (!prim) {
        MString errorMsg("Failed to create prim for USD reference assembly at path: ");
        errorMsg += MString(authorPath.GetText());
        MGlobal::displayError(errorMsg);
        return false;
    }

    // only write references when time is default
    if (!usdTime.IsDefault()) {
        return true;
    }

    const MDagPath& currPath = args.GetMDagPath();

    // because of how we generate these things and node collapsing, sometimes
    // the currPath is for the USD reference assembly and some times it's for
    // the USD proxy shape.
    const MFnDagNode assemblyNode(currPath.transform());

    MStatus status;
    MPlug usdRefFilepathPlg = assemblyNode.findPlug(
        _tokens->FilePathPlugName.GetText(), &status);
    if (status == MS::kSuccess) {
        UsdReferences refs = prim.GetReferences();
        std::string refAssetPath(usdRefFilepathPlg.asString().asChar());

        std::string resolvedRefPath =
            stage->ResolveIdentifierToEditTarget(refAssetPath);

        if (!resolvedRefPath.empty()) {
            std::string refPrimPathStr;
            MPlug usdRefPrimPathPlg = assemblyNode.findPlug(
                _tokens->PrimPathPlugName.GetText(), &status);
            if (status == MS::kSuccess) {
                refPrimPathStr = usdRefPrimPathPlg.asString().asChar();
            }

            if (refPrimPathStr.empty()) {
                refs.AppendReference(refAssetPath);
            } else {
                SdfPath refPrimPath(refPrimPathStr);

                if (refPrimPath.IsRootPrimPath()) {
                    refs.AppendReference(SdfReference(refAssetPath, refPrimPath));
                } else {
                    MString errorMsg("Not creating reference for assembly node '");
                    errorMsg += assemblyNode.fullPathName();
                    errorMsg += "' with non-root prim path: ";
                    errorMsg += refPrimPath.GetText();
                    MGlobal::displayError(errorMsg);
                }
            }
        } else {
            MString errorMsg("Could not resolve reference '");
            errorMsg += refAssetPath.c_str();
            errorMsg += "'; creating placeholder Xform for <";
            errorMsg += authorPath.GetText();
            errorMsg += ">";
            MGlobal::displayWarning(errorMsg);
            prim.SetDocumentation(std::string(errorMsg.asChar()));
        }
    }

    auto registeredVariantSets = UsdUtilsGetRegisteredVariantSets();
    if (!registeredVariantSets.empty()) {
        // import variant selections: we only import the "persistent" ones.
        for (const auto& regVarSet: registeredVariantSets) {
            switch (regVarSet.selectionExportPolicy) {
                case UsdUtilsRegisteredVariantSet::SelectionExportPolicy::Never:
                case UsdUtilsRegisteredVariantSet::SelectionExportPolicy::IfAuthored:
                    continue;
                case UsdUtilsRegisteredVariantSet::SelectionExportPolicy::Always:
                    break;
            }

            const std::string& variantSetName = regVarSet.name;
            std::string variantSetPlugName = TfStringPrintf("%s%s",
                _tokens->VariantSetPlugNamePrefix.GetText(), variantSetName.c_str());

            MPlug modelingVariantPlg = assemblyNode.findPlug(variantSetPlugName.c_str(), &status);
            if (status == MS::kSuccess) {
                MString variant;
                modelingVariantPlg.getValue(variant);
                prim.GetVariantSet(variantSetName).SetVariantSelection(variant.asChar());
            }
        }
    }
    else {
        // export all that we can.
        if (UsdMayaReferenceAssembly* usdRefAssem = 
            dynamic_cast<UsdMayaReferenceAssembly*>(assemblyNode.userNode())) {
            for (const auto& varSels: usdRefAssem->GetVariantSetSelections()) {
                const std::string& variantSetName = varSels.first;
                const std::string& variant = varSels.second;
                prim.GetVariantSet(variantSetName).SetVariantSelection(variant);
            }
        }
    }

    bool makeInstanceable = args.GetExportRefsAsInstanceable();
    if (makeInstanceable) {
        // When bug/128076 is addressed, the IsGroup() check will become
        // unnecessary and obsolete.
        // XXX This test also needs to fail if there are sub-root overs
        // on the referenceAssembly!
        TfToken kind;
        UsdModelAPI(prim).GetKind(&kind);
        if (!prim.HasAuthoredInstanceable() &&
            !KindRegistry::GetInstance().IsA(kind, KindTokens->group)) {
            prim.SetInstanceable(true);
        }
    }

    return true;
}

static
bool
_HasAssetInfo(const UsdPrim& prim)
{
    UsdModelAPI usdModel(prim);
    SdfAssetPath identifier;
    if (!usdModel.GetAssetIdentifier(&identifier)) {
        return false;
    }

    return true;
}

static
bool
_HasReferenceInfo(const UsdPrim& prim)
{
    SdfReferenceListOp refs;
    prim.GetMetadata(SdfFieldKeys->References, &refs);

    // this logic is not robust.  awaiting bug 99278.
    if (!refs.GetAddedItems().empty()) {
        return true;
    }
    if (!refs.GetExplicitItems().empty()) {
        return true;
    }

    return false;
}

/* static */
bool
PxrUsdMayaTranslatorModelAssembly::ShouldImportAsAssembly(
    const UsdPrim& usdImportRootPrim,
    const UsdPrim& prim)
{
    if (!prim) {
        return false;
    }

    if (!prim.IsModel()) {
        return false;
    }

    if (prim == usdImportRootPrim) {
        return false;
    }

    // First we check if we're bringing in an asset (and not a reference to an
    // asset).
    if (_HasAssetInfo(prim)) {
        return true;
    }

    // If we can't find any assetInfo, fall back to checking the reference.
    if (_HasReferenceInfo(prim)) {
        return true;
    }

    return false;
}

static
std::map<std::string, std::string>
_GetVariantSelections(const UsdPrim& prim)
{
    std::map<std::string, std::string> varSels;
    UsdVariantSets varSets = prim.GetVariantSets();
    std::vector<std::string> varSetNames = varSets.GetNames();
    TF_FOR_ALL(iter, varSetNames) {
        const std::string& varSetName = *iter;
        std::string varSel = varSets.GetVariantSelection(varSetName);
        if (!varSel.empty()) {
            varSels[varSetName] = varSel;
        }
    }
    return varSels;
}

/* static */
bool
PxrUsdMayaTranslatorModelAssembly::Read(
    const UsdPrim& prim,
    const std::string& assetIdentifier,
    const SdfPath& assetPrimPath,
    MObject parentNode,
    const PxrUsdMayaPrimReaderArgs& args,
    PxrUsdMayaPrimReaderContext* context,
    const std::string& assemblyTypeName,
    const std::string& assemblyRep,
    const std::vector<std::string>& parentRefs)
{
    UsdStageCacheContext stageCacheContext(UsdMayaStageCache::Get());
    UsdStageRefPtr usdStage = UsdStage::Open(assetIdentifier);
    if (!usdStage) {
        MGlobal::displayError("Cannot open USD file " +
            MString(assetIdentifier.c_str()));
        return false;
    }

    UsdPrim modelPrim;
    if (!assetPrimPath.IsEmpty()) {
        modelPrim = usdStage->GetPrimAtPath(assetPrimPath);
    } else {
        modelPrim = usdStage->GetDefaultPrim();
    }

    if (!modelPrim) {
        MGlobal::displayError("Could not find model prim in USD file " +
            MString(assetIdentifier.c_str()));
        return false;
    }


#if (PIXMAYA_FORCE_MAYA_REFERENCES)
    // The primitivePath and the topLayerUsd are passed in as options in the
    // option string; note that this is all the information USD actually needs /
    // uses... the refPath is actually just a dummy path we need for maya's
    // referencing system. It needs a path that actually exists on disk, and it
    // needs to not be the same as the parent reference (or else maya freaks
    // outs, trying to recursively load the ref).

    // Currently, we're just using the next file on the layer stack...

    // (Wanted to look at references, but apparently that's not easy to get?
    // GetMetadata('references') seems to return an empty list, and
    // UsdPrim.GetReferences() has this note:
    //    /// Return a UsdReferences object that allows one to add, remove, or
    //    /// mutate references <em>at the currently set UsdEditTarget</em>.
    //    ///
    //    /// There is currently no facility for \em listing the currently authored
    //    /// references on a prim... the problem is somewhat ill-defined, and
    //    /// requires some thought.

    MStatus status;
    MFnDagNode parentMFn(parentNode, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    MString parentName = parentMFn.partialPathName();

    std::string refPath;

    auto isParentPath = [](const std::string& layerPath, const std::vector<std::string>& parentPaths) -> bool {
        for (unsigned int i = 0; i < parentPaths.size(); ++i)
        {
            if (parentPaths[i].empty()) {
                continue;
            }
            if (layerPath == parentPaths[i]) {
                return true;
            }
        }
        return false;
    };

    // For a more general purpose case, we would have only taken a vector of
    // strings and a delimiter... but in our case, we will always be adding
    // a new "lastString", so may as well assume this for speed
    auto joinStrs = [](const std::vector<std::string>& strings,
                      const std::string& lastString,
                      const char delimiter, std::string& result) {
        result.clear();
        unsigned int new_len = 0;

        auto addStrLen = [](unsigned int& len, const std::string& str) {
            if (!str.empty()) {
                // +1 for the delimiter character
                if (len != 0)
                    ++len;
                len += str.length();
            }
        };

        for (auto& str : strings) {
            addStrLen(new_len, str);
        }
        addStrLen(new_len, lastString);

        result.reserve(new_len);

        auto appendStr = [](std::string& aggregate, const std::string& str,
                            const char delim) {
            if (!str.empty()) {
                if (!aggregate.empty())
                    aggregate += delim;
                aggregate += str;
            }
        };

        for (auto& str : strings) {
            appendStr(result, str, delimiter);
        }
        appendStr(result, lastString, delimiter);
    };

    for (const auto& layerSpec : prim.GetPrimStack()) {
        const std::string& layerPath = layerSpec->GetLayer()->GetRealPath();
        if (layerPath.empty())
            continue;
        if (!isParentPath(layerPath, parentRefs))
        {
            refPath = layerPath;
            break;
        }
    }
    if (!refPath.length())
    {
        MString errorMsg("Failed to find a non-recursive reference path for ");
        errorMsg += prim.GetPath().GetText();
        errorMsg += " in top-level usd file ";
        errorMsg += usdStage->GetRootLayer()->GetRealPath().c_str();
        MGlobal::displayError(errorMsg);
        return false;
    }

    // Don't know of a way to pass in an option string using MFileIO::reference,
    // so just uisng mel...
    std::string joinedParentRefs;
    joinStrs(parentRefs, refPath, ',', joinedParentRefs);
    MString cmd = (MString("file -reference -options \"primPath=")
                   // TODO: properly escape these strings
                   // Pass in the primitive path...
                   + prim.GetPath().GetText()
                   // ...and the top-level usd file in the option string.
                   // Note that that's all the information that USD actually
                   // needs / uses - the refPath is actually just a dummy. See
                   // note above where we generate the refPath.
                   + ";topLayerUsd="
                   + assetIdentifier.c_str()
                   + ";parent="
                   + parentName
                   + ";parentRefPaths="
                   + joinedParentRefs.c_str()
                   + ";readAnimData="
                   + int(args.GetReadAnimData())
                   + ";startTime="
                   + args.GetStartTime()
                   + ";endTime="
                   + args.GetEndTime()
                   + "\" \""
                   + refPath.c_str()
                   + "\";");
    DEBUG_PRINT(cmd);
    CHECK_MSTATUS_AND_RETURN(MGlobal::executeCommand(cmd), false);

#else // PIXMAYA_FORCE_MAYA_REFERENCES
    // We have to create the new assembly node with the assembly command as
    // opposed to using MDagModifier's createNode() or any other method. That
    // seems to be the only way to ensure that the assembly's namespace and
    // container are setup correctly.
    const std::string assemblyCmd =
        TfStringPrintf("cmds.assembly(name=\'%s\', type=\'%s\')",
                       prim.GetName().GetText(),
                       assemblyTypeName.c_str());
    MString newAssemblyName;
    MStatus status = MGlobal::executePythonCommand(assemblyCmd.c_str(),
                                                   newAssemblyName);
    CHECK_MSTATUS_AND_RETURN(status, false);

    // Now we get the MObject for the assembly node we just created.
    MObject assemblyObj;
    status = PxrUsdMayaUtil::GetMObjectByName(newAssemblyName.asChar(),
                                              assemblyObj);
    CHECK_MSTATUS_AND_RETURN(status, false);

    // Re-parent the assembly node underneath parentNode.
    MDagModifier dagMod;
    status = dagMod.reparentNode(assemblyObj, parentNode);
    CHECK_MSTATUS_AND_RETURN(status, false);

    // Read xformable attributes from the UsdPrim on to the assembly node.
    UsdGeomXformable xformable(prim);
    PxrUsdMayaTranslatorXformable::Read(xformable, assemblyObj, args, context);

    MFnDependencyNode depNodeFn(assemblyObj, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);

    // Set the filePath and primPath attributes.
    MPlug filePathPlug = depNodeFn.findPlug(_tokens->FilePathPlugName.GetText(),
        true, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    status = dagMod.newPlugValueString(filePathPlug, assetIdentifier.c_str());
    CHECK_MSTATUS_AND_RETURN(status, false);

    MPlug primPathPlug = depNodeFn.findPlug(_tokens->PrimPathPlugName.GetText(),
        true, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    status = dagMod.newPlugValueString(primPathPlug, modelPrim.GetPath().GetText());
    CHECK_MSTATUS_AND_RETURN(status, false);

    // Set the kind attribute.
    TfToken modelKind;
    UsdModelAPI usdModel(modelPrim);
    if (!usdModel.GetKind(&modelKind) || modelKind.IsEmpty()) {
        modelKind = KindTokens->component;
    }

    MPlug kindPlug = depNodeFn.findPlug(_tokens->KindPlugName.GetText(), true, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    status = dagMod.newPlugValueString(kindPlug, modelKind.GetText());
    CHECK_MSTATUS_AND_RETURN(status, false);

    // Apply variant selections.
    std::map<std::string, std::string> variantSelections = _GetVariantSelections(prim);
    TF_FOR_ALL(iter, variantSelections) {
        std::string variantSetName = iter->first;
        std::string variantSelection = iter->second;

        std::string variantSetPlugName = TfStringPrintf("%s%s",
            _tokens->VariantSetPlugNamePrefix.GetText(), variantSetName.c_str());
        MPlug varSetPlug = depNodeFn.findPlug(variantSetPlugName.c_str(), true, &status);
        if (status != MStatus::kSuccess) {
            MFnTypedAttribute typedAttrFn;
            MObject attrObj = typedAttrFn.create(variantSetPlugName.c_str(),
                                                 variantSetPlugName.c_str(),
                                                 MFnData::kString,
                                                 MObject::kNullObj,
                                                 &status);
            CHECK_MSTATUS_AND_RETURN(status, false);
            status = depNodeFn.addAttribute(attrObj);
            CHECK_MSTATUS_AND_RETURN(status, false);
            varSetPlug = depNodeFn.findPlug(variantSetPlugName.c_str(), true, &status);
            CHECK_MSTATUS_AND_RETURN(status, false);
        }
        status = dagMod.newPlugValueString(varSetPlug, variantSelection.c_str());
        CHECK_MSTATUS_AND_RETURN(status, false);
    }

    status = dagMod.doIt();
    CHECK_MSTATUS_AND_RETURN(status, false);

    if (context) {
        context->RegisterNewMayaNode(prim.GetPath().GetString(),
                                     assemblyObj);
    }

    // If a representation was supplied, activate it.
    if (!assemblyRep.empty()) {
        MFnAssembly assemblyFn(assemblyObj, &status);
        CHECK_MSTATUS_AND_RETURN(status, false);
        if (assemblyFn.canActivate(&status)) {
            status = assemblyFn.activate(assemblyRep.c_str());
            CHECK_MSTATUS_AND_RETURN(status, false);
        }
    }

#endif // PIXMAYA_FORCE_MAYA_REFERENCES

    if (context) {
        context->SetPruneChildren(true);
    }

    // XXX: right now, we lose any edits that may be introduced from
    // the current file on top of the asset we're bringing as an assembly.
    // see bug 125359.

    return true;
}

/* static */
bool
PxrUsdMayaTranslatorModelAssembly::ReadAsProxy(
    const UsdPrim& prim,
    const std::map<std::string, std::string>& variantSetSelections,
    MObject parentNode,
    const PxrUsdMayaPrimReaderArgs& args,
    PxrUsdMayaPrimReaderContext* context,
    const std::string& proxyShapeTypeName)
{
    if (!prim) {
        return false;
    }

    const SdfPath primPath = prim.GetPath();

    MStatus status;

    // Create a transform node for the proxy node under its parent node.
    MObject transformObj;
    if (!PxrUsdMayaTranslatorUtil::CreateTransformNode(prim,
                                                          parentNode,
                                                          args,
                                                          context,
                                                          &status,
                                                          &transformObj)) {
        return false;
    }

    // Create the proxy shape node.
    MDagModifier dagMod;
    MObject proxyObj = dagMod.createNode(proxyShapeTypeName.c_str(),
                                         transformObj,
                                         &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    status = dagMod.doIt();
    CHECK_MSTATUS_AND_RETURN(status, false);
    TF_VERIFY(!proxyObj.isNull());
    const std::string proxyShapeNodeName = TfStringPrintf("%s%s",
            prim.GetName().GetText(), _tokens->MayaProxyShapeNameSuffix.GetText());
    status = dagMod.renameNode(proxyObj, proxyShapeNodeName.c_str());
    CHECK_MSTATUS_AND_RETURN(status, false);
    if (context) {
        const SdfPath shapePrimPath = primPath.AppendChild(TfToken(proxyShapeNodeName));
        context->RegisterNewMayaNode(shapePrimPath.GetString(), proxyObj);
    }

    // Set the filePath and primPath attributes.
    MFnDependencyNode depNodeFn(proxyObj, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    MPlug filePathPlug = depNodeFn.findPlug(_tokens->FilePathPlugName.GetText(),
        true, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    const std::string rootLayerRealPath = prim.GetStage()->GetRootLayer()->GetRealPath();
    status = dagMod.newPlugValueString(filePathPlug, rootLayerRealPath.c_str());
    CHECK_MSTATUS_AND_RETURN(status, false);

    MPlug primPathPlug = depNodeFn.findPlug(_tokens->PrimPathPlugName.GetText(),
        true, &status);
    CHECK_MSTATUS_AND_RETURN(status, false);
    status = dagMod.newPlugValueString(primPathPlug, primPath.GetText());
    CHECK_MSTATUS_AND_RETURN(status, false);

    // XXX: For now, the proxy shape only support modelingVariant with the
    // 'variantKey' attribute. Eventually, it should support any/all
    // variantSets.
    const std::map<std::string, std::string>::const_iterator varSetIter =
        variantSetSelections.find(_tokens->modelingVariant.GetString());
    if (varSetIter != variantSetSelections.end()) {
        const std::string modelingVariantSelection = varSetIter->second;
        MPlug variantKeyPlug = depNodeFn.findPlug(_tokens->variantKey.GetText(),
            true, &status);
        CHECK_MSTATUS_AND_RETURN(status, false);
        status = dagMod.newPlugValueString(variantKeyPlug,
            modelingVariantSelection.c_str());
        CHECK_MSTATUS_AND_RETURN(status, false);
    }

    status = dagMod.doIt();
    CHECK_MSTATUS_AND_RETURN(status, false);

    if (context) {
        context->SetPruneChildren(true);
    }

    return true;
}

PXR_NAMESPACE_CLOSE_SCOPE

