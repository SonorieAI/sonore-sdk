// SPDX-License-Identifier: Apache-2.0
// The LV2 TTL generator for a plugin built with sonore_add_plugin(): the
// plugin object already holds the generator (sonore_lv2_ttlgen_main in
// lv2_wrapper.h); this is the main() that calls it. See sonore_add_lv2's
// OBJECTS branch in SonoreSDKHelpers.cmake for why it is a separate file.
extern "C" int sonore_lv2_ttlgen_main(int argc, char** argv);
int main(int argc, char** argv) { return sonore_lv2_ttlgen_main(argc, argv); }
