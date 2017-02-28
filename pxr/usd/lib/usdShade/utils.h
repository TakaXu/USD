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
#ifndef USD_SHD_UTILS_H
#define USD_SHD_UTILS_H

#include "pxr/pxr.h"
#include "pxr/usd/usdShade/api.h"

#include "pxr/base/tf/token.h"

#include <string>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE


/// \enum UsdShadeAttributeType
/// 
/// Specifies the type of a shading attribute.
/// 
/// "Parameter" and "InterfaceAttribute" are deprecated shading attribute types.
/// 
enum class UsdShadeAttributeType {
    Input,
    Output,
    Parameter,
    InterfaceAttribute
};

/// \class UsdShadeUtils
///
/// This class contains a set of utility functions used when authoring and 
/// querying shading networks.
///
class UsdShadeUtils {
public:
    /// Returns the namespace prefix of the USD attribute associated with the given
    /// shading attribute type.
    USDSHADE_API
    static std::string GetPrefixForAttributeType(
        UsdShadeAttributeType sourceType);

    /// Given the full name of a shading property, returns it's base name and type.
    USDSHADE_API
    static std::pair<TfToken, UsdShadeAttributeType> 
        GetBaseNameAndType(const TfToken &fullName);

    /// Returns the full shading attribute name given the basename and the type.
    USDSHADE_API
    static TfToken GetFullName(const TfToken &baseName, 
                                    const UsdShadeAttributeType type);

    /// Whether the env-setting that enables the reading of old-style encoding 
    /// of shading networks is set to 'true'.
    USDSHADE_API
    static bool ReadOldEncoding();

    /// Whether the env-setting that enables the writing of new-style encoding 
    /// of shading networks is set to 'true'.
    USDSHADE_API
    static bool WriteNewEncoding();
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif
