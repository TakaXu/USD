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
#include "pxr/usd/usdAi/aiNodeAPI.h"
#include "pxr/usd/usd/schemaRegistry.h"
#include "pxr/usd/usd/typed.h"

#include "pxr/usd/sdf/types.h"
#include "pxr/usd/sdf/assetPath.h"

PXR_NAMESPACE_OPEN_SCOPE

// Register the schema with the TfType system.
TF_REGISTRY_FUNCTION(TfType)
{
    TfType::Define<UsdAiNodeAPI,
        TfType::Bases< UsdSchemaBase > >();
    
}

/* virtual */
UsdAiNodeAPI::~UsdAiNodeAPI()
{
}

/* static */
UsdAiNodeAPI
UsdAiNodeAPI::Get(const UsdStagePtr &stage, const SdfPath &path)
{
    if (!stage) {
        TF_CODING_ERROR("Invalid stage");
        return UsdAiNodeAPI();
    }
    return UsdAiNodeAPI(stage->GetPrimAtPath(path));
}


/* static */
const TfType &
UsdAiNodeAPI::_GetStaticTfType()
{
    static TfType tfType = TfType::Find<UsdAiNodeAPI>();
    return tfType;
}

/* static */
bool 
UsdAiNodeAPI::_IsTypedSchema()
{
    static bool isTyped = _GetStaticTfType().IsA<UsdTyped>();
    return isTyped;
}

/* virtual */
const TfType &
UsdAiNodeAPI::_GetTfType() const
{
    return _GetStaticTfType();
}

/*static*/
const TfTokenVector&
UsdAiNodeAPI::GetSchemaAttributeNames(bool includeInherited)
{
    static TfTokenVector localNames;
    static TfTokenVector allNames =
        UsdSchemaBase::GetSchemaAttributeNames(true);

    if (includeInherited)
        return allNames;
    else
        return localNames;
}

PXR_NAMESPACE_CLOSE_SCOPE

// ===================================================================== //
// Feel free to add custom code below this line. It will be preserved by
// the code generator.
//
// Just remember to wrap code in the pxr namespace macros:
// PXR_NAMESPACE_OPEN_SCOPE, PXR_NAMESPACE_CLOSE_SCOPE.
// ===================================================================== //
// --(BEGIN CUSTOM CODE)--

PXR_NAMESPACE_OPEN_SCOPE

#include "pxr/usd/usdAi/tokens.h"

UsdAttribute
UsdAiNodeAPI::CreateUserAttribute(const TfToken &name,
                                  const SdfValueTypeName& typeName) const
{
    return GetPrim().CreateAttribute(
        TfToken(UsdAiTokens->userPrefix.GetString() + name.GetString()),
        typeName);
}

UsdAttribute
UsdAiNodeAPI::GetUserAttribute(const TfToken &name) const
{
    return GetPrim().GetAttribute(
        TfToken(UsdAiTokens->userPrefix.GetString() + name.GetString()));
}

std::vector<UsdAttribute>
UsdAiNodeAPI::GetUserAttributes() const
{
    std::vector<UsdAttribute> result;
    std::vector<UsdAttribute> attrs = GetPrim().GetAttributes();

    TF_FOR_ALL(attrIter, attrs) {
        const UsdAttribute& attr = *attrIter;
        if (TfStringStartsWith(attr.GetName().GetString(),
                               UsdAiTokens->userPrefix)) {
            result.push_back(attr);
        }
    }
    return result;
}

PXR_NAMESPACE_CLOSE_SCOPE
