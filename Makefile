# vrdual PoC build. Vulkan headers are vendored in third_party/ (no system
# vulkan-headers package needed); the loader is linked from the system.
B        := build
CXX      ?= g++
CXXFLAGS ?= -O2 -g -Wall -Wextra -Wno-missing-field-initializers
CXXFLAGS += -std=c++17 -Ithird_party/Vulkan-Headers/include
SDL      := $(shell pkg-config --cflags --libs sdl3)
VKXML    := third_party/Vulkan-Headers/registry/vk.xml
# The layer is loaded into foreign processes (Steam runtime, Flatpak): no
# dependency on the host's C++ runtime.
LAYERFLAGS := -shared -fPIC -fvisibility=hidden -static-libstdc++ -static-libgcc -I$(B)
LAYERSRC := layer/vrdual_layer.cpp layer/vrdual_mirror.hpp layer/vrdual_cursor.hpp layer/spirv_patch.hpp common/vrdual_shm.h common/vrdual_config.hpp common/vrdual_ctl.hpp $(B)/mirror_gen.hpp

all: $(B)/libVkLayer_vrdual.so $(B)/VkLayer_vrdual.json $(B)/vrdual-cube $(B)/vrdual-gltest $(B)/vrdual-compositor $(B)/vrdual-xr $(B)/spvpatch $(B)/vrdual-ctl $(B)/vrdual-input-test

# 32-bit layer for 32-bit games (Zen Bound 2, most older Steam titles).
lib32: $(B)/lib32/libVkLayer_vrdual.so $(B)/lib32/VkLayer_vrdual.json $(B)/lib32/vrdual-gltest

$(B) $(B)/lib32:
	mkdir -p $@

$(B)/mirror_gen.hpp: tools/gen_mirror.py $(VKXML) | $(B)
	python3 -I tools/gen_mirror.py $(VKXML) $(B)/mirror_dispatch.hpp $@

$(B)/libVkLayer_vrdual.so: $(LAYERSRC) | $(B)
	$(CXX) $(CXXFLAGS) $(LAYERFLAGS) -o $@ $< -lrt

$(B)/lib32/libVkLayer_vrdual.so: $(LAYERSRC) | $(B)/lib32
	$(CXX) -m32 $(CXXFLAGS) $(LAYERFLAGS) -o $@ $< -lrt

$(B)/lib32/vrdual-gltest: testapp/gltest.cpp | $(B)/lib32
	$(CXX) -m32 $(CXXFLAGS) -o $@ $< -lGL -lX11

$(B)/VkLayer_vrdual.json: layer/VkLayer_vrdual.json.in | $(B)
	cp $< $@

$(B)/lib32/VkLayer_vrdual.json: layer/VkLayer_vrdual.json.in | $(B)/lib32
	cp $< $@

$(B)/%.inc: testapp/% | $(B)
	glslc -mfmt=c $< -o $@

$(B)/vrdual-cube: testapp/main.cpp $(B)/cube.vert.inc $(B)/cube_bda.vert.inc $(B)/cube.frag.inc | $(B)
	$(CXX) $(CXXFLAGS) -I$(B) -o $@ $< -lvulkan $(SDL)

$(B)/vrdual-gltest: testapp/gltest.cpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< -lGL -lX11

$(B)/vrdual-compositor: compositor/main.cpp common/vrdual_shm.h common/vrdual_ctl.hpp common/vrdual_frames.hpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< $(SDL) -lrt

$(B)/vrdual-xr: compositor/xr.cpp common/vrdual_frames.hpp common/vrdual_ctl.hpp common/vrdual_input.hpp $(B)/input_names.hpp | $(B)
	$(CXX) $(CXXFLAGS) -I$(B) -Ithird_party/OpenXR-SDK/include -o $@ $< -lvulkan -ldl

# KEY_*/BTN_* names for map_<button> = ... (values resolved by the compiler).
$(B)/input_names.hpp: /usr/include/linux/input-event-codes.h | $(B)
	awk '/^#define (KEY|BTN)_[A-Z0-9_]+[ \t]/ && $$2 != "KEY_MAX" && $$2 != "KEY_CNT" {print "{\"" $$2 "\", " $$2 "},"}' $< > $@

$(B)/vrdual-input-test: tools/input_test.cpp common/vrdual_input.hpp common/vrdual_frames.hpp common/vrdual_config.hpp $(B)/input_names.hpp | $(B)
	$(CXX) $(CXXFLAGS) -I$(B) -o $@ $<

$(B)/spvpatch: tools/spvpatch.cpp layer/spirv_patch.hpp | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $<

$(B)/vrdual-ctl: tools/vrdual_ctl.cpp common/vrdual_config.hpp common/vrdual_ctl.hpp common/vrdual_shm.h | $(B)
	$(CXX) $(CXXFLAGS) -o $@ $< -lrt

clean:
	rm -rf $(B)

.PHONY: all lib32 clean
