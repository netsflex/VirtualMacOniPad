#pragma once
#import <Metal/Metal.h>
#include <stdbool.h>

// Skip guest draws and dispatches whose shaders dereference guest GPU
// addresses, which the iPadOS 16 host cannot honor. See the source for why.
bool VZModernInstallAddressGuard(id<MTLDevice> device);
// A converted library whose function loads an address from memory.
void VZModernMarkAddressLibrary(id library);
// Marks a new pipeline when any of its functions came from a marked library.
// source is the pipeline descriptor, or the function for a compute pipeline.
void VZModernMarkAddressPipeline(id pipeline, id source);
