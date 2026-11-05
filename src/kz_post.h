#pragma once

// Switches for the game's own full-screen post-processing passes (docs/findings.md "Post-process passes").
// The hooks wrap the game functions that draw the passes, so a disabled pass is never submitted to the GS.

class PS2Runtime;

void kzPostInstall(PS2Runtime &runtime);
// LOD detail multiplier (Graphics.LodScale / KZ_LOD_SCALE; a netplay session sets the host's value)
void kzPostSetLodScale(float scale);
float kzPostLodScale();
