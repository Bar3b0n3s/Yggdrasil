// The instance records of a mesh draw (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; Shared/InstanceRecord.h,
// Shared/DrawConstants.h): the one place a program that draws meshes (Passes/Scene.slang, Passes/Shadow.slang) finds the
// record of instance i of its draw, included after it declares Instances (t10, the view's records), StaticInstances (t21,
// the GPU-resident records of the static set, Renderer/GpuResourceCache.h), InstanceIndices (t22, the view's indices into
// them) and Draw (the push constants), which it reads by name.
//
// A dynamic draw's instance i is the record Instances[Draw.FirstInstance + i]. A static draw (DrawConstants::Flags with
// DrawConstantsStaticInstances) reads the static record StaticInstances[InstanceIndices[Draw.FirstInstance + i].Record]. The
// vertex shader hands the fragment shaders the record's index with StaticInstanceIndexBit set for a static record (a static
// record's index is below 2^31, the static pick ids' range, Renderer/RenderStaticSet.h), and every read of a record, the
// vertex shader's and the fragment shaders', goes through LoadInstanceRecord, so a static instance reads its own record
// wherever a dynamic one reads its.
#pragma once

#include "Shared/DrawConstants.h"
#include "Shared/InstanceRecord.h"

// The bit of an instance index that marks a static record (StaticInstances) rather than a view's record (Instances).
static const uint StaticInstanceIndexBit = 0x80000000u;

// The index of instance `instance` of the draw, StaticInstanceIndexBit set for a static draw's. SV_VulkanInstanceID is
// gl_InstanceIndex itself, which counts from 0 within the draw since every draw's own first instance is 0; SV_InstanceID would
// subtract the base instance and need the DrawParameters capability, which needs the shaderDrawParameters device feature.
uint GetInstanceIndex(uint instance)
{
	if ((Draw.Flags & DrawConstantsStaticInstances) != 0)
		return InstanceIndices[Draw.FirstInstance + instance].Record | StaticInstanceIndexBit;
	return Draw.FirstInstance + instance;
}

// The record of instance index `index` (GetInstanceIndex's).
InstanceRecord LoadInstanceRecord(uint index)
{
	if ((index & StaticInstanceIndexBit) != 0)
		return StaticInstances[index & ~StaticInstanceIndexBit];
	return Instances[index];
}
