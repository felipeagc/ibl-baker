# IBL baker

Simple IBL baker written in C99 using vulkan.

It converts .hdr equirectangular environment maps into cubemaps
for IBL irradiance and radiance (with roughness mipmaps).

## TODO
- [ ] BRDF LUT generation
- [ ] Command line argument parsing
- [ ] Option for putting all faces/mipmaps in one .hdr image
