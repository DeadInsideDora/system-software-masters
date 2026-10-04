LAB3_BUILD_DIR ?= build

.PHONY: all lab3 demo demo-lab3 profile profile-lab3 setup-vm setup-plots clean
all: lab3
lab3:
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" lab3
demo demo-lab3: lab3
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" demo
profile profile-lab3: lab3
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" profile
setup-vm:
	$(MAKE) -C lab3 setup-vm
clean:
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" clean

setup-plots:
	$(MAKE) -C lab3 setup-plots

.PHONY: demo-animals
demo-animals: lab3
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" demo-animals

.PHONY: profile-copying profile-animals
profile-copying profile-animals: lab3
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" $@

.PHONY: demo-blackbox profile-blackbox
demo-blackbox profile-blackbox: lab3
	$(MAKE) -C lab3 BUILD_DIR="$(LAB3_BUILD_DIR)" $@
