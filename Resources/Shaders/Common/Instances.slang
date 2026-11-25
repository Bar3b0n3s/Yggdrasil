// The instance records of a mesh draw (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; Shared/InstanceRecord.h,
// Shared/DrawConstants.h): the one place a program that draws meshes (Passes/Scene.slang, Passes/Shadow.slang) finds the
// record of instance i of its draw, included after it declares Instances (t10, the view's records), StaticInstances (t21,
// the GPU-resident records of the static set, Renderer/GpuResourceCache.h), InstanceIndices (t22, the view's indices into
// them) and Draw (the push constants), which it reads by name.
//
// A dynamic draw's instance i is the record Instances[Draw.FirstInstance + i]. A static draw (DrawConstants::Flags with
// DrawConstantsStaticInstances) reads the static record StaticInstances[InstanceIndices[Draw.FirstInstance + i].Record]. The
// vertex shader hands the fragment shaders the instance's index, StaticInstanceIndexBit set for a static instance (M46: its
// InstanceIndices entry's index; below 2^31, as a static record's, the static pick ids' range, Renderer/RenderStaticSet.h),
// and every read of a record, the vertex shader's and the fragment shaders', goes through LoadInstanceRecord, so a static
// instance reads its own record wherever a dynamic one reads its.
//
// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3E and its W46.4 record; ADR 0060's phase-8
// ledger, amended 2026-11-25): a static instance's level fade is its view's, InstanceIndices[i].Reserved (the bits of a
// signed float), and its wind its draw's, since a static record is shared by every view and every submesh of its item.
// So GetInstanceIndex hands the fragment shaders a static instance's entry index i (with StaticInstanceIndexBit), and
// LoadInstanceRecord returns the static record with LodFade from that entry and, for a draw with DrawConstantsWind (in every
// stage that loads the record; only the vertex stage displaces), DrawFlagWind and the draw's WindResponse; a dynamic record
// is returned as it is.
#pragma once

#include "Shared/DrawConstants.h"
#include "Shared/InstanceRecord.h"

// The bit of an instance index that marks a static record (StaticInstances) rather than a view's record (Instances).
static const uint StaticInstanceIndexBit = 0x80000000u;

// The index of instance `instance` of the draw, StaticInstanceIndexBit set for a static draw's (M46: its InstanceIndices
// entry's, which holds its record's index and its view's fade). SV_VulkanInstanceID is gl_InstanceIndex itself, which counts
// from 0 within the draw since every draw's own first instance is 0; SV_InstanceID would subtract the base instance and need
// the DrawParameters capability, which needs the shaderDrawParameters device feature.
uint GetInstanceIndex(uint instance)
{
	if ((Draw.Flags & DrawConstantsStaticInstances) != 0)
		return (Draw.FirstInstance + instance) | StaticInstanceIndexBit;
	return Draw.FirstInstance + instance;
}

// The record of instance index `index` (GetInstanceIndex's). M46: a static record with its entry's LodFade and, under
// DrawConstantsWind, the draw's wind (the file comment).
InstanceRecord LoadInstanceRecord(uint index)
{
	if ((index & StaticInstanceIndexBit) != 0)
	{
		const InstanceIndex entry = InstanceIndices[index & ~StaticInstanceIndexBit];
		InstanceRecord record = StaticInstances[entry.Record];
		record.LodFade = asfloat(entry.Reserved);
		if ((Draw.Flags & DrawConstantsWind) != 0)
		{
			record.Flags |= DrawFlagWind;
			record.WindResponse = Draw.WindResponse;
		}
		return record;
	}
	return Instances[index];
}
