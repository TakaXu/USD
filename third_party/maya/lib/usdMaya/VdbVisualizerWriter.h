#ifndef _usdExport_VdbVisualizerWriter_h_
#define _usdExport_VdbVisualizerWriter_h_

#include "pxr/pxr.h"
#include "usdMaya/MayaTransformWriter.h"

PXR_NAMESPACE_OPEN_SCOPE

class VdbVisualizerWriter : public MayaTransformWriter {
public:
    VdbVisualizerWriter(const MDagPath& iDag, UsdStageRefPtr stage, const JobExportArgs& iArgs);
    virtual ~VdbVisualizerWriter();

    virtual void write(const UsdTimeCode& usdTime) override;
private:
    bool has_velocity_grids;
};

typedef shared_ptr<VdbVisualizerWriter> VdbVisualizerWriterPtr;

PXR_NAMESPACE_CLOSE_SCOPE

#endif
