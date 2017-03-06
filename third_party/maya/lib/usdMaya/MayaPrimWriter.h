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
#ifndef PXRUSDMAYA_MAYAPRIMWRITER_H
#define PXRUSDMAYA_MAYAPRIMWRITER_H
#include "pxr/pxr.h"
#include "usdMaya/usdWriteJobCtx.h"

#include "pxr/usd/usd/stage.h"

PXR_NAMESPACE_OPEN_SCOPE

class UsdGeomImageable;
class UsdTimeCode;

// Writes an MFnMesh as a poly mesh OR a subd mesh
class MayaPrimWriter
{
  public:
    MayaPrimWriter(
            const MDagPath& iDag,
            const SdfPath& uPath,
            usdWriteJobCtx& job);
    virtual ~MayaPrimWriter() {};

    virtual void write(const UsdTimeCode &usdTime) = 0;
    virtual bool isShapeAnimated()     const = 0;

    /// Does this PrimWriter directly create one or more gprims on the UsdStage?
    ///
    /// Base implementation returns \c false, so gprim/shape-derived classes
    /// should override.
    virtual bool exportsGprims() const;
    
    /// Does this PrimWriter add references on the UsdStage?
    ///
    /// Base implementation returns \c false.
    virtual bool exportsReferences() const;

    /// Does this PrimWriter request that the traversal code skip its child
    /// nodes because this PrimWriter will handle its child nodes by itself?
    ///
    /// Base implementation returns \c false.
    virtual bool shouldPruneChildren() const;

public:
    const MDagPath&        getDagPath()    const { return mDagPath; }
    const SdfPath&         getUsdPath()    const { return mUsdPath; }
    const UsdStageRefPtr&  getUsdStage()   const { return mJob.getUsdStage(); }
    bool isValid()                         const { return mIsValid; }
    const JobExportArgs&   getArgs()       const { return mJob.getArgs(); }
    const UsdPrim&         getPrim()       const { return mUsdPrim; }


protected:
    void setValid(bool isValid) { mIsValid = isValid; }
    void setUsdPath(const SdfPath &newPath) { mUsdPath = newPath; }
    bool writePrimAttrs(const MDagPath & iDag2, const UsdTimeCode &usdTime, UsdGeomImageable &primSchema);

    UsdPrim mUsdPrim;
    usdWriteJobCtx& mJob;

private:
    MDagPath mDagPath;
    SdfPath mUsdPath;

    bool mIsValid;
};

PXR_NAMESPACE_CLOSE_SCOPE

#endif // PXRUSDMAYA_MAYAPRIMWRITER_H
