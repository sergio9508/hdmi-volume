BUILD_DIR := build
DRIVER_BUNDLE := $(BUILD_DIR)/HDMIVolume.driver
APP_BUNDLE := $(BUILD_DIR)/HDMIVolume.app

.PHONY: all driver app test clean

all: driver app

driver:
	@mkdir -p $(DRIVER_BUNDLE)/Contents/MacOS
	@cp Driver/Info.plist $(DRIVER_BUNDLE)/Contents/Info.plist
	clang -Wall -Wextra -Werror -std=c11 -bundle -I Driver \
		Driver/dsp_loopback.c Driver/HDMIVolumePlugIn.c \
		-framework CoreAudio -framework CoreFoundation -lm \
		-o $(DRIVER_BUNDLE)/Contents/MacOS/HDMIVolume
	@codesign --force --sign - $(DRIVER_BUNDLE)

app:
	cd App && swift build -c release
	@mkdir -p $(APP_BUNDLE)/Contents/MacOS $(APP_BUNDLE)/Contents/Resources
	@cp App/Info.plist $(APP_BUNDLE)/Contents/Info.plist
	@cp App/Resources/AppIcon.icns $(APP_BUNDLE)/Contents/Resources/AppIcon.icns
	@cp App/.build/release/HDMIVolume $(APP_BUNDLE)/Contents/MacOS/HDMIVolume
	@codesign --force --sign - $(APP_BUNDLE)

test:
	@mkdir -p $(BUILD_DIR)
	clang -Wall -Wextra -Werror -std=c11 -I Driver Driver/dsp_loopback.c Tests/test_dsp_loopback.c -lm -o $(BUILD_DIR)/test_dsp_loopback
	./$(BUILD_DIR)/test_dsp_loopback
	cd App && swift test

clean:
	rm -rf $(BUILD_DIR) App/.build
