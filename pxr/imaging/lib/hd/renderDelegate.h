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
#ifndef HD_RENDER_DELEGATE_H
#define HD_RENDER_DELEGATE_H

#include "pxr/base/tf/declarePtrs.h"
#include "pxr/base/tf/refPtr.h"
#include "pxr/base/tf/weakPtr.h"

#include <string>

TF_DECLARE_WEAK_AND_REF_PTRS(HdRenderDelegate);

/// \class HdRenderDelegate
///
class HdRenderDelegate
    : public TfRefBase
    , public TfWeakBase
{
public:

    ///
    /// This class is not intended to be copied.
    ///
    HdRenderDelegate(const HdRenderDelegate &) = delete;
    HdRenderDelegate &operator=(const HdRenderDelegate &) = delete;

    ///
    /// Returns a string lable that is appropriate to use in a UI menu, for
    /// example.
    ///
    /// XXX: Note that this is an example only and something like the label
    /// for the render delegate should be provided by plugin metadata so
    /// that we don't have to load the plugin to provide it as an option.
    ///
    virtual std::string GetLabel() const = 0;



protected:

    HdRenderDelegate();

    virtual ~HdRenderDelegate();

};

#endif //HD_RENDER_DELEGATE_H
