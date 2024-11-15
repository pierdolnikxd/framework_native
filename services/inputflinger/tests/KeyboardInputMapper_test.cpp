/*
 * Copyright 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "KeyboardInputMapper.h"

#include <cstdint>
#include <list>
#include <memory>
#include <optional>
#include <string>

#include <android/input.h>
#include <android/keycodes.h>
#include <com_android_input_flags.h>
#include <flag_macros.h>
#include <ftl/flags.h>
#include <gtest/gtest.h>
#include <input/DisplayViewport.h>
#include <input/Input.h>
#include <input/InputDevice.h>
#include <ui/LogicalDisplayId.h>
#include <ui/Rotation.h>

#include "EventHub.h"
#include "InputMapperTest.h"
#include "InterfaceMocks.h"
#include "NotifyArgs.h"
#include "TestConstants.h"
#include "TestEventMatchers.h"

#define TAG "KeyboardInputMapper_test"

namespace android {

using testing::_;
using testing::AllOf;
using testing::Args;
using testing::DoAll;
using testing::Return;
using testing::SetArgPointee;
using testing::VariantWith;

namespace {

// Arbitrary display properties.
constexpr ui::LogicalDisplayId DISPLAY_ID = ui::LogicalDisplayId::DEFAULT;
constexpr int32_t DISPLAY_WIDTH = 480;
constexpr int32_t DISPLAY_HEIGHT = 800;
constexpr std::optional<uint8_t> NO_PORT = std::nullopt; // no physical port is specified

} // namespace

/**
 * Unit tests for KeyboardInputMapper.
 */
class KeyboardInputMapperUnitTest : public InputMapperUnitTest {
protected:
    sp<FakeInputReaderPolicy> mFakePolicy;
    const std::unordered_map<int32_t, int32_t> mKeyCodeMap{{KEY_0, AKEYCODE_0},
                                                           {KEY_A, AKEYCODE_A},
                                                           {KEY_LEFTCTRL, AKEYCODE_CTRL_LEFT},
                                                           {KEY_LEFTALT, AKEYCODE_ALT_LEFT},
                                                           {KEY_RIGHTALT, AKEYCODE_ALT_RIGHT},
                                                           {KEY_LEFTSHIFT, AKEYCODE_SHIFT_LEFT},
                                                           {KEY_RIGHTSHIFT, AKEYCODE_SHIFT_RIGHT},
                                                           {KEY_FN, AKEYCODE_FUNCTION},
                                                           {KEY_LEFTCTRL, AKEYCODE_CTRL_LEFT},
                                                           {KEY_RIGHTCTRL, AKEYCODE_CTRL_RIGHT},
                                                           {KEY_LEFTMETA, AKEYCODE_META_LEFT},
                                                           {KEY_RIGHTMETA, AKEYCODE_META_RIGHT},
                                                           {KEY_CAPSLOCK, AKEYCODE_CAPS_LOCK},
                                                           {KEY_NUMLOCK, AKEYCODE_NUM_LOCK},
                                                           {KEY_SCROLLLOCK, AKEYCODE_SCROLL_LOCK}};

    void SetUp() override {
        InputMapperUnitTest::SetUp();

        // set key-codes expected in tests
        for (const auto& [scanCode, outKeycode] : mKeyCodeMap) {
            EXPECT_CALL(mMockEventHub, mapKey(EVENTHUB_ID, scanCode, _, _, _, _, _))
                    .WillRepeatedly(DoAll(SetArgPointee<4>(outKeycode), Return(NO_ERROR)));
        }

        mFakePolicy = sp<FakeInputReaderPolicy>::make();
        EXPECT_CALL(mMockInputReaderContext, getPolicy).WillRepeatedly(Return(mFakePolicy.get()));

        ON_CALL((*mDevice), getSources).WillByDefault(Return(AINPUT_SOURCE_KEYBOARD));

        mMapper = createInputMapper<KeyboardInputMapper>(*mDeviceContext, mReaderConfiguration,
                                                         AINPUT_SOURCE_KEYBOARD);
    }
};

TEST_F(KeyboardInputMapperUnitTest, KeyPressTimestampRecorded) {
    nsecs_t when = ARBITRARY_TIME;
    std::vector<int32_t> keyCodes{KEY_0, KEY_A, KEY_LEFTCTRL, KEY_RIGHTALT, KEY_LEFTSHIFT};
    EXPECT_CALL(mMockInputReaderContext, setLastKeyDownTimestamp)
            .With(Args<0>(when))
            .Times(keyCodes.size());
    for (int32_t keyCode : keyCodes) {
        process(when, EV_KEY, keyCode, 1);
        process(when, EV_SYN, SYN_REPORT, 0);
        process(when, EV_KEY, keyCode, 0);
        process(when, EV_SYN, SYN_REPORT, 0);
    }
}

TEST_F(KeyboardInputMapperUnitTest, RepeatEventsDiscarded) {
    std::list<NotifyArgs> args;
    args += process(ARBITRARY_TIME, EV_KEY, KEY_0, 1);
    args += process(ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);

    args += process(ARBITRARY_TIME, EV_KEY, KEY_0, 2);
    args += process(ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);

    args += process(ARBITRARY_TIME, EV_KEY, KEY_0, 0);
    args += process(ARBITRARY_TIME, EV_SYN, SYN_REPORT, 0);

    EXPECT_THAT(args,
                ElementsAre(VariantWith<NotifyKeyArgs>(AllOf(WithKeyAction(AKEY_EVENT_ACTION_DOWN),
                                                             WithKeyCode(AKEYCODE_0),
                                                             WithScanCode(KEY_0))),
                            VariantWith<NotifyKeyArgs>(AllOf(WithKeyAction(AKEY_EVENT_ACTION_UP),
                                                             WithKeyCode(AKEYCODE_0),
                                                             WithScanCode(KEY_0)))));
}

// TODO(b/283812079): convert the tests below to use InputMapperUnitTest.

// --- KeyboardInputMapperTest ---

class KeyboardInputMapperTest : public InputMapperTest {
protected:
    void SetUp() override {
        InputMapperTest::SetUp(DEVICE_CLASSES | InputDeviceClass::KEYBOARD |
                               InputDeviceClass::ALPHAKEY);
    }
    const std::string UNIQUE_ID = "local:0";
    const KeyboardLayoutInfo DEVICE_KEYBOARD_LAYOUT_INFO = KeyboardLayoutInfo("en-US", "qwerty");
    void prepareDisplay(ui::Rotation orientation);

    void testDPadKeyRotation(KeyboardInputMapper& mapper, int32_t originalScanCode,
                             int32_t originalKeyCode, int32_t rotatedKeyCode,
                             ui::LogicalDisplayId displayId = ui::LogicalDisplayId::INVALID);
};

/* Similar to setDisplayInfoAndReconfigure, but pre-populates all parameters except for the
 * orientation.
 */
void KeyboardInputMapperTest::prepareDisplay(ui::Rotation orientation) {
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, orientation, UNIQUE_ID,
                                 NO_PORT, ViewportType::INTERNAL);
}

void KeyboardInputMapperTest::testDPadKeyRotation(KeyboardInputMapper& mapper,
                                                  int32_t originalScanCode, int32_t originalKeyCode,
                                                  int32_t rotatedKeyCode,
                                                  ui::LogicalDisplayId displayId) {
    NotifyKeyArgs args;

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, originalScanCode, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(originalScanCode, args.scanCode);
    ASSERT_EQ(rotatedKeyCode, args.keyCode);
    ASSERT_EQ(displayId, args.displayId);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, originalScanCode, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(originalScanCode, args.scanCode);
    ASSERT_EQ(rotatedKeyCode, args.keyCode);
    ASSERT_EQ(displayId, args.displayId);
}

TEST_F(KeyboardInputMapperTest, GetSources) {
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, mapper.getSources());
}

TEST_F(KeyboardInputMapperTest, Process_SimpleKeyPress) {
    const int32_t USAGE_A = 0x070004;
    const int32_t USAGE_UNKNOWN = 0x07ffff;
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, USAGE_A, AKEYCODE_A, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, KEY_NUMLOCK, AKEYCODE_NUM_LOCK, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, KEY_CAPSLOCK, AKEYCODE_CAPS_LOCK, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, KEY_SCROLLLOCK, AKEYCODE_SCROLL_LOCK, POLICY_FLAG_WAKE);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    // Initial metastate is AMETA_NONE.
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());

    // Key down by scan code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_HOME, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(AKEYCODE_HOME, args.keyCode);
    ASSERT_EQ(KEY_HOME, args.scanCode);
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    ASSERT_EQ(ARBITRARY_TIME, args.downTime);

    // Key up by scan code.
    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME + 1, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(AKEYCODE_HOME, args.keyCode);
    ASSERT_EQ(KEY_HOME, args.scanCode);
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    ASSERT_EQ(ARBITRARY_TIME, args.downTime);

    // Key down by usage code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_MSC, MSC_SCAN, USAGE_A);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, 0, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(AKEYCODE_A, args.keyCode);
    ASSERT_EQ(0, args.scanCode);
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    ASSERT_EQ(ARBITRARY_TIME, args.downTime);

    // Key up by usage code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_MSC, MSC_SCAN, USAGE_A);
    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, 0, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME + 1, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(AKEYCODE_A, args.keyCode);
    ASSERT_EQ(0, args.scanCode);
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
    ASSERT_EQ(ARBITRARY_TIME, args.downTime);

    // Key down with unknown scan code or usage code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_MSC, MSC_SCAN, USAGE_UNKNOWN);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UNKNOWN, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(0, args.keyCode);
    ASSERT_EQ(KEY_UNKNOWN, args.scanCode);
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);
    ASSERT_EQ(0U, args.policyFlags);
    ASSERT_EQ(ARBITRARY_TIME, args.downTime);

    // Key up with unknown scan code or usage code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_MSC, MSC_SCAN, USAGE_UNKNOWN);
    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_UNKNOWN, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME + 1, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(0, args.keyCode);
    ASSERT_EQ(KEY_UNKNOWN, args.scanCode);
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);
    ASSERT_EQ(0U, args.policyFlags);
    ASSERT_EQ(ARBITRARY_TIME, args.downTime);
}

TEST_F(KeyboardInputMapperTest, Process_KeyRemapping) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_A, 0, AKEYCODE_A, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_B, 0, AKEYCODE_B, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    mFakeEventHub->setKeyRemapping(EVENTHUB_ID, {{AKEYCODE_A, AKEYCODE_B}});
    // Key down by scan code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_A, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEYCODE_B, args.keyCode);

    // Key up by scan code.
    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_A, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEYCODE_B, args.keyCode);
}

/**
 * Ensure that the readTime is set to the time when the EV_KEY is received.
 */
TEST_F(KeyboardInputMapperTest, Process_SendsReadTime) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    NotifyKeyArgs args;

    // Key down
    process(mapper, ARBITRARY_TIME, /*readTime=*/12, EV_KEY, KEY_HOME, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(12, args.readTime);

    // Key up
    process(mapper, ARBITRARY_TIME, /*readTime=*/15, EV_KEY, KEY_HOME, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(15, args.readTime);
}

TEST_F(KeyboardInputMapperTest, Process_ShouldUpdateMetaState) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFTSHIFT, 0, AKEYCODE_SHIFT_LEFT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_A, 0, AKEYCODE_A, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, KEY_NUMLOCK, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, KEY_CAPSLOCK, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, KEY_SCROLLLOCK, AKEYCODE_SCROLL_LOCK, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    // Initial metastate is AMETA_NONE.
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());

    // Metakey down.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_LEFTSHIFT, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON, args.metaState);
    ASSERT_EQ(AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON, mapper.getMetaState());
    ASSERT_NO_FATAL_FAILURE(mReader->getContext()->assertUpdateGlobalMetaStateWasCalled());

    // Key down.
    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_A, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON, args.metaState);
    ASSERT_EQ(AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON, mapper.getMetaState());

    // Key up.
    process(mapper, ARBITRARY_TIME + 2, READ_TIME, EV_KEY, KEY_A, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON, args.metaState);
    ASSERT_EQ(AMETA_SHIFT_LEFT_ON | AMETA_SHIFT_ON, mapper.getMetaState());

    // Metakey up.
    process(mapper, ARBITRARY_TIME + 3, READ_TIME, EV_KEY, KEY_LEFTSHIFT, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AMETA_NONE, args.metaState);
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());
    ASSERT_NO_FATAL_FAILURE(mReader->getContext()->assertUpdateGlobalMetaStateWasCalled());
}

TEST_F(KeyboardInputMapperTest, Process_WhenNotOrientationAware_ShouldNotRotateDPad) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    prepareDisplay(ui::ROTATION_90);
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper,
                                                KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper,
                                                KEY_RIGHT, AKEYCODE_DPAD_RIGHT, AKEYCODE_DPAD_RIGHT));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper,
                                                KEY_DOWN, AKEYCODE_DPAD_DOWN, AKEYCODE_DPAD_DOWN));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper,
                                                KEY_LEFT, AKEYCODE_DPAD_LEFT, AKEYCODE_DPAD_LEFT));
}

TEST_F(KeyboardInputMapperTest, Process_WhenOrientationAware_ShouldRotateDPad) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);

    addConfigurationProperty("keyboard.orientationAware", "1");
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    prepareDisplay(ui::ROTATION_0);
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));

    clearViewports();
    prepareDisplay(ui::ROTATION_90);
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));

    clearViewports();
    prepareDisplay(ui::ROTATION_180);
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));

    clearViewports();
    prepareDisplay(ui::ROTATION_270);
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_UP, DISPLAY_ID));

    // Special case: if orientation changes while key is down, we still emit the same keycode
    // in the key up as we did in the key down.
    NotifyKeyArgs args;
    clearViewports();
    prepareDisplay(ui::ROTATION_270);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(KEY_UP, args.scanCode);
    ASSERT_EQ(AKEYCODE_DPAD_RIGHT, args.keyCode);

    clearViewports();
    prepareDisplay(ui::ROTATION_180);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(KEY_UP, args.scanCode);
    ASSERT_EQ(AKEYCODE_DPAD_RIGHT, args.keyCode);
}

TEST_F(KeyboardInputMapperTest, DisplayIdConfigurationChange_NotOrientationAware) {
    // If the keyboard is not orientation aware,
    // key events should not be associated with a specific display id
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    NotifyKeyArgs args;

    // Display id should be LogicalDisplayId::INVALID without any display configuration.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(ui::LogicalDisplayId::INVALID, args.displayId);

    prepareDisplay(ui::ROTATION_0);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(ui::LogicalDisplayId::INVALID, args.displayId);
}

TEST_F(KeyboardInputMapperTest, DisplayIdConfigurationChange_OrientationAware) {
    // If the keyboard is orientation aware,
    // key events should be associated with the internal viewport
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);

    addConfigurationProperty("keyboard.orientationAware", "1");
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    NotifyKeyArgs args;

    // Display id should be LogicalDisplayId::INVALID without any display configuration.
    // ^--- already checked by the previous test

    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui::ROTATION_0,
                                 UNIQUE_ID, NO_PORT, ViewportType::INTERNAL);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DISPLAY_ID, args.displayId);

    constexpr ui::LogicalDisplayId newDisplayId = ui::LogicalDisplayId{2};
    clearViewports();
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui::ROTATION_0,
                                 UNIQUE_ID, NO_PORT, ViewportType::INTERNAL);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_UP, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(newDisplayId, args.displayId);
}

TEST_F(KeyboardInputMapperTest, GetKeyCodeState) {
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    mFakeEventHub->setKeyCodeState(EVENTHUB_ID, AKEYCODE_A, 1);
    ASSERT_EQ(1, mapper.getKeyCodeState(AINPUT_SOURCE_ANY, AKEYCODE_A));

    mFakeEventHub->setKeyCodeState(EVENTHUB_ID, AKEYCODE_A, 0);
    ASSERT_EQ(0, mapper.getKeyCodeState(AINPUT_SOURCE_ANY, AKEYCODE_A));
}

TEST_F(KeyboardInputMapperTest, GetKeyCodeForKeyLocation) {
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    mFakeEventHub->addKeyCodeMapping(EVENTHUB_ID, AKEYCODE_Y, AKEYCODE_Z);
    ASSERT_EQ(AKEYCODE_Z, mapper.getKeyCodeForKeyLocation(AKEYCODE_Y))
                                << "If a mapping is available, the result is equal to the mapping";

    ASSERT_EQ(AKEYCODE_A, mapper.getKeyCodeForKeyLocation(AKEYCODE_A))
                                << "If no mapping is available, the result is the key location";
}

TEST_F(KeyboardInputMapperTest, GetScanCodeState) {
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    mFakeEventHub->setScanCodeState(EVENTHUB_ID, KEY_A, 1);
    ASSERT_EQ(1, mapper.getScanCodeState(AINPUT_SOURCE_ANY, KEY_A));

    mFakeEventHub->setScanCodeState(EVENTHUB_ID, KEY_A, 0);
    ASSERT_EQ(0, mapper.getScanCodeState(AINPUT_SOURCE_ANY, KEY_A));
}

TEST_F(KeyboardInputMapperTest, MarkSupportedKeyCodes) {
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    mFakeEventHub->addKey(EVENTHUB_ID, KEY_A, 0, AKEYCODE_A, 0);

    uint8_t flags[2] = { 0, 0 };
    ASSERT_TRUE(mapper.markSupportedKeyCodes(AINPUT_SOURCE_ANY, {AKEYCODE_A, AKEYCODE_B}, flags));
    ASSERT_TRUE(flags[0]);
    ASSERT_FALSE(flags[1]);
}

TEST_F(KeyboardInputMapperTest, Process_LockedKeysShouldToggleMetaStateAndLeds) {
    mFakeEventHub->addLed(EVENTHUB_ID, LED_CAPSL, true /*initially on*/);
    mFakeEventHub->addLed(EVENTHUB_ID, LED_NUML, false /*initially off*/);
    mFakeEventHub->addLed(EVENTHUB_ID, LED_SCROLLL, false /*initially off*/);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_CAPSLOCK, 0, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_NUMLOCK, 0, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_SCROLLLOCK, 0, AKEYCODE_SCROLL_LOCK, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    // Initial metastate is AMETA_NONE.
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());

    // Initialization should have turned all of the lights off.
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));

    // Toggle caps lock on.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON, mapper.getMetaState());

    // Toggle num lock on.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON, mapper.getMetaState());

    // Toggle caps lock off.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_NUM_LOCK_ON, mapper.getMetaState());

    // Toggle scroll lock on.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_NUM_LOCK_ON | AMETA_SCROLL_LOCK_ON, mapper.getMetaState());

    // Toggle num lock off.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_SCROLL_LOCK_ON, mapper.getMetaState());

    // Toggle scroll lock off.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());
}

TEST_F(KeyboardInputMapperTest, Configure_AssignsDisplayPort) {
    // keyboard 1.
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);

    // keyboard 2.
    const std::string USB2 = "USB2";
    const std::string DEVICE_NAME2 = "KEYBOARD2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    std::shared_ptr<InputDevice> device2 =
            newDevice(SECOND_DEVICE_ID, DEVICE_NAME2, USB2, SECOND_EVENTHUB_ID,
                      ftl::Flags<InputDeviceClass>(0));

    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_UP, 0, AKEYCODE_DPAD_UP, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_RIGHT, 0, AKEYCODE_DPAD_RIGHT, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    device2->addEmptyEventHubDevice(SECOND_EVENTHUB_ID);
    KeyboardInputMapper& mapper2 =
            device2->constructAndAddMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID,
                                                                mFakePolicy
                                                                        ->getReaderConfiguration(),
                                                                AINPUT_SOURCE_KEYBOARD);
    std::list<NotifyArgs> unused =
            device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                    /*changes=*/{});
    unused += device2->reset(ARBITRARY_TIME);

    // Prepared displays and associated info.
    constexpr uint8_t hdmi1 = 0;
    constexpr uint8_t hdmi2 = 1;
    const std::string SECONDARY_UNIQUE_ID = "local:1";

    mFakePolicy->addInputPortAssociation(DEVICE_LOCATION, hdmi1);
    mFakePolicy->addInputPortAssociation(USB2, hdmi2);

    // No associated display viewport found, should disable the device.
    unused += device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                                 InputReaderConfiguration::Change::DISPLAY_INFO);
    ASSERT_FALSE(device2->isEnabled());

    // Prepare second display.
    constexpr ui::LogicalDisplayId newDisplayId = ui::LogicalDisplayId{2};
    setDisplayInfoAndReconfigure(DISPLAY_ID, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui::ROTATION_0,
                                 UNIQUE_ID, hdmi1, ViewportType::INTERNAL);
    setDisplayInfoAndReconfigure(newDisplayId, DISPLAY_WIDTH, DISPLAY_HEIGHT, ui::ROTATION_0,
                                 SECONDARY_UNIQUE_ID, hdmi2, ViewportType::EXTERNAL);
    // Default device will reconfigure above, need additional reconfiguration for another device.
    unused += device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                                 InputReaderConfiguration::Change::DISPLAY_INFO);

    // Device should be enabled after the associated display is found.
    ASSERT_TRUE(mDevice->isEnabled());
    ASSERT_TRUE(device2->isEnabled());

    // Test pad key events
    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, DISPLAY_ID));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, DISPLAY_ID));

    ASSERT_NO_FATAL_FAILURE(
            testDPadKeyRotation(mapper2, KEY_UP, AKEYCODE_DPAD_UP, AKEYCODE_DPAD_UP, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_RIGHT, AKEYCODE_DPAD_RIGHT,
                                                AKEYCODE_DPAD_RIGHT, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_DOWN, AKEYCODE_DPAD_DOWN,
                                                AKEYCODE_DPAD_DOWN, newDisplayId));
    ASSERT_NO_FATAL_FAILURE(testDPadKeyRotation(mapper2, KEY_LEFT, AKEYCODE_DPAD_LEFT,
                                                AKEYCODE_DPAD_LEFT, newDisplayId));
}

TEST_F(KeyboardInputMapperTest, Process_LockedKeysShouldToggleAfterReattach) {
    mFakeEventHub->addLed(EVENTHUB_ID, LED_CAPSL, true /*initially on*/);
    mFakeEventHub->addLed(EVENTHUB_ID, LED_NUML, false /*initially off*/);
    mFakeEventHub->addLed(EVENTHUB_ID, LED_SCROLLL, false /*initially off*/);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_CAPSLOCK, 0, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_NUMLOCK, 0, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_SCROLLLOCK, 0, AKEYCODE_SCROLL_LOCK, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    // Initial metastate is AMETA_NONE.
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());

    // Initialization should have turned all of the lights off.
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));

    // Toggle caps lock on.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON, mapper.getMetaState());

    // Toggle num lock on.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON, mapper.getMetaState());

    // Toggle scroll lock on.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 1);
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON | AMETA_SCROLL_LOCK_ON, mapper.getMetaState());

    mFakeEventHub->removeDevice(EVENTHUB_ID);
    mReader->loopOnce();

    // keyboard 2 should default toggle keys.
    const std::string USB2 = "USB2";
    const std::string DEVICE_NAME2 = "KEYBOARD2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    std::shared_ptr<InputDevice> device2 =
            newDevice(SECOND_DEVICE_ID, DEVICE_NAME2, USB2, SECOND_EVENTHUB_ID,
                      ftl::Flags<InputDeviceClass>(0));
    mFakeEventHub->addLed(SECOND_EVENTHUB_ID, LED_CAPSL, true /*initially on*/);
    mFakeEventHub->addLed(SECOND_EVENTHUB_ID, LED_NUML, false /*initially off*/);
    mFakeEventHub->addLed(SECOND_EVENTHUB_ID, LED_SCROLLL, false /*initially off*/);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_CAPSLOCK, 0, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_NUMLOCK, 0, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_SCROLLLOCK, 0, AKEYCODE_SCROLL_LOCK, 0);

    device2->addEmptyEventHubDevice(SECOND_EVENTHUB_ID);
    KeyboardInputMapper& mapper2 =
            device2->constructAndAddMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID,
                                                                mFakePolicy
                                                                        ->getReaderConfiguration(),
                                                                AINPUT_SOURCE_KEYBOARD);
    std::list<NotifyArgs> unused =
            device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                    /*changes=*/{});
    unused += device2->reset(ARBITRARY_TIME);

    ASSERT_TRUE(mFakeEventHub->getLedState(SECOND_EVENTHUB_ID, LED_CAPSL));
    ASSERT_TRUE(mFakeEventHub->getLedState(SECOND_EVENTHUB_ID, LED_NUML));
    ASSERT_TRUE(mFakeEventHub->getLedState(SECOND_EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON | AMETA_NUM_LOCK_ON | AMETA_SCROLL_LOCK_ON,
              mapper2.getMetaState());
}

TEST_F(KeyboardInputMapperTest, Process_toggleCapsLockState) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_CAPSLOCK, 0, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_NUMLOCK, 0, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_SCROLLLOCK, 0, AKEYCODE_SCROLL_LOCK, 0);

    // Suppose we have two mappers. (DPAD + KEYBOARD)
    constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_DPAD);
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    // Initial metastate is AMETA_NONE.
    ASSERT_EQ(AMETA_NONE, mapper.getMetaState());

    mReader->toggleCapsLockState(DEVICE_ID);
    ASSERT_EQ(AMETA_CAPS_LOCK_ON, mapper.getMetaState());
}

TEST_F(KeyboardInputMapperTest, Process_LockedKeysShouldToggleInMultiDevices) {
    // keyboard 1.
    mFakeEventHub->addLed(EVENTHUB_ID, LED_CAPSL, true /*initially on*/);
    mFakeEventHub->addLed(EVENTHUB_ID, LED_NUML, false /*initially off*/);
    mFakeEventHub->addLed(EVENTHUB_ID, LED_SCROLLL, false /*initially off*/);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_CAPSLOCK, 0, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_NUMLOCK, 0, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_SCROLLLOCK, 0, AKEYCODE_SCROLL_LOCK, 0);

    KeyboardInputMapper& mapper1 =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    // keyboard 2.
    const std::string USB2 = "USB2";
    const std::string DEVICE_NAME2 = "KEYBOARD2";
    constexpr int32_t SECOND_DEVICE_ID = DEVICE_ID + 1;
    constexpr int32_t SECOND_EVENTHUB_ID = EVENTHUB_ID + 1;
    std::shared_ptr<InputDevice> device2 =
            newDevice(SECOND_DEVICE_ID, DEVICE_NAME2, USB2, SECOND_EVENTHUB_ID,
                      ftl::Flags<InputDeviceClass>(0));
    mFakeEventHub->addLed(SECOND_EVENTHUB_ID, LED_CAPSL, true /*initially on*/);
    mFakeEventHub->addLed(SECOND_EVENTHUB_ID, LED_NUML, false /*initially off*/);
    mFakeEventHub->addLed(SECOND_EVENTHUB_ID, LED_SCROLLL, false /*initially off*/);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_CAPSLOCK, 0, AKEYCODE_CAPS_LOCK, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_NUMLOCK, 0, AKEYCODE_NUM_LOCK, 0);
    mFakeEventHub->addKey(SECOND_EVENTHUB_ID, KEY_SCROLLLOCK, 0, AKEYCODE_SCROLL_LOCK, 0);

    device2->addEmptyEventHubDevice(SECOND_EVENTHUB_ID);
    KeyboardInputMapper& mapper2 =
            device2->constructAndAddMapper<KeyboardInputMapper>(SECOND_EVENTHUB_ID,
                                                                mFakePolicy
                                                                        ->getReaderConfiguration(),
                                                                AINPUT_SOURCE_KEYBOARD);
    std::list<NotifyArgs> unused =
            device2->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                    /*changes=*/{});
    unused += device2->reset(ARBITRARY_TIME);

    // Initial metastate is AMETA_NONE.
    ASSERT_EQ(AMETA_NONE, mapper1.getMetaState());
    ASSERT_EQ(AMETA_NONE, mapper2.getMetaState());

    // Toggle num lock on and off.
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 1);
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_EQ(AMETA_NUM_LOCK_ON, mapper1.getMetaState());
    ASSERT_EQ(AMETA_NUM_LOCK_ON, mapper2.getMetaState());

    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 1);
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_NUMLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_NUML));
    ASSERT_EQ(AMETA_NONE, mapper1.getMetaState());
    ASSERT_EQ(AMETA_NONE, mapper2.getMetaState());

    // Toggle caps lock on and off.
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 1);
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_EQ(AMETA_CAPS_LOCK_ON, mapper1.getMetaState());
    ASSERT_EQ(AMETA_CAPS_LOCK_ON, mapper2.getMetaState());

    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 1);
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_CAPSLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_CAPSL));
    ASSERT_EQ(AMETA_NONE, mapper1.getMetaState());
    ASSERT_EQ(AMETA_NONE, mapper2.getMetaState());

    // Toggle scroll lock on and off.
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 1);
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 0);
    ASSERT_TRUE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_SCROLL_LOCK_ON, mapper1.getMetaState());
    ASSERT_EQ(AMETA_SCROLL_LOCK_ON, mapper2.getMetaState());

    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 1);
    process(mapper1, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_SCROLLLOCK, 0);
    ASSERT_FALSE(mFakeEventHub->getLedState(EVENTHUB_ID, LED_SCROLLL));
    ASSERT_EQ(AMETA_NONE, mapper1.getMetaState());
    ASSERT_EQ(AMETA_NONE, mapper2.getMetaState());
}

TEST_F(KeyboardInputMapperTest, Process_DisabledDevice) {
    const int32_t USAGE_A = 0x070004;
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, 0, USAGE_A, AKEYCODE_A, POLICY_FLAG_WAKE);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    // Key down by scan code.
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_HOME, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(DEVICE_ID, args.deviceId);
    ASSERT_EQ(AINPUT_SOURCE_KEYBOARD, args.source);
    ASSERT_EQ(ARBITRARY_TIME, args.eventTime);
    ASSERT_EQ(AKEY_EVENT_ACTION_DOWN, args.action);
    ASSERT_EQ(AKEYCODE_HOME, args.keyCode);
    ASSERT_EQ(KEY_HOME, args.scanCode);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM, args.flags);

    // Disable device, it should synthesize cancellation events for down events.
    mFakePolicy->addDisabledDevice(DEVICE_ID);
    configureDevice(InputReaderConfiguration::Change::ENABLED_STATE);

    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_ACTION_UP, args.action);
    ASSERT_EQ(AKEYCODE_HOME, args.keyCode);
    ASSERT_EQ(KEY_HOME, args.scanCode);
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM | AKEY_EVENT_FLAG_CANCELED, args.flags);
}

TEST_F(KeyboardInputMapperTest, Configure_AssignKeyboardLayoutInfo) {
    constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    std::list<NotifyArgs> unused =
            mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                    /*changes=*/{});

    uint32_t generation = mReader->getContext()->getGeneration();
    mFakePolicy->addKeyboardLayoutAssociation(DEVICE_LOCATION, DEVICE_KEYBOARD_LAYOUT_INFO);

    unused += mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                                 InputReaderConfiguration::Change::KEYBOARD_LAYOUT_ASSOCIATION);

    InputDeviceInfo deviceInfo = mDevice->getDeviceInfo();
    ASSERT_EQ(DEVICE_KEYBOARD_LAYOUT_INFO.languageTag,
              deviceInfo.getKeyboardLayoutInfo()->languageTag);
    ASSERT_EQ(DEVICE_KEYBOARD_LAYOUT_INFO.layoutType,
              deviceInfo.getKeyboardLayoutInfo()->layoutType);
    ASSERT_TRUE(mReader->getContext()->getGeneration() != generation);

    // Call change layout association with the same values: Generation shouldn't change
    generation = mReader->getContext()->getGeneration();
    mFakePolicy->addKeyboardLayoutAssociation(DEVICE_LOCATION, DEVICE_KEYBOARD_LAYOUT_INFO);
    unused += mDevice->configure(ARBITRARY_TIME, mFakePolicy->getReaderConfiguration(),
                                 InputReaderConfiguration::Change::KEYBOARD_LAYOUT_ASSOCIATION);
    ASSERT_TRUE(mReader->getContext()->getGeneration() == generation);
}

TEST_F(KeyboardInputMapperTest, LayoutInfoCorrectlyMapped) {
    mFakeEventHub->setRawLayoutInfo(EVENTHUB_ID,
                                    RawLayoutInfo{.languageTag = "en", .layoutType = "extended"});

    // Configuration
    constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    InputReaderConfiguration config;
    std::list<NotifyArgs> unused = mDevice->configure(ARBITRARY_TIME, config, /*changes=*/{});

    ASSERT_EQ("en", mDevice->getDeviceInfo().getKeyboardLayoutInfo()->languageTag);
    ASSERT_EQ("extended", mDevice->getDeviceInfo().getKeyboardLayoutInfo()->layoutType);
}

TEST_F(KeyboardInputMapperTest, Process_GesureEventToSetFlagKeepTouchMode) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_LEFT, 0, AKEYCODE_DPAD_LEFT, POLICY_FLAG_GESTURE);
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);
    NotifyKeyArgs args;

    // Key down
    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_LEFT, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(AKEY_EVENT_FLAG_FROM_SYSTEM | AKEY_EVENT_FLAG_KEEP_TOUCH_MODE, args.flags);
}

TEST_F_WITH_FLAGS(KeyboardInputMapperTest, WakeBehavior_AlphabeticKeyboard,
        REQUIRES_FLAGS_ENABLED(ACONFIG_FLAG(com::android::input::flags,
                                            enable_alphabetic_keyboard_wake))) {
    // For internal alphabetic devices, keys will trigger wake on key down.

    mFakeEventHub->addKey(EVENTHUB_ID, KEY_A, 0, AKEYCODE_A, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAYPAUSE, 0, AKEYCODE_MEDIA_PLAY_PAUSE, 0);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_A, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_A, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_HOME, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_PLAYPAUSE, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_PLAYPAUSE, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);
}

/**
 * When there is more than one KeyboardInputMapper for an InputDevice, each mapper should produce
 * events that use the shared keyboard source across all mappers. This is to ensure that each
 * input device generates key events in a consistent manner, regardless of which mapper produces
 * the event.
 */
TEST_F(KeyboardInputMapperTest, UsesSharedKeyboardSource) {
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);

    // Add a mapper with SOURCE_KEYBOARD
    KeyboardInputMapper& keyboardMapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    process(keyboardMapper, ARBITRARY_TIME, 0, EV_KEY, KEY_HOME, 1);
    ASSERT_NO_FATAL_FAILURE(
            mFakeListener->assertNotifyKeyWasCalled(WithSource(AINPUT_SOURCE_KEYBOARD)));
    process(keyboardMapper, ARBITRARY_TIME, 0, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(
            mFakeListener->assertNotifyKeyWasCalled(WithSource(AINPUT_SOURCE_KEYBOARD)));

    // Add a mapper with SOURCE_DPAD
    KeyboardInputMapper& dpadMapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_DPAD);
    for (auto* mapper : {&keyboardMapper, &dpadMapper}) {
        process(*mapper, ARBITRARY_TIME, 0, EV_KEY, KEY_HOME, 1);
        ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(
                WithSource(AINPUT_SOURCE_KEYBOARD | AINPUT_SOURCE_DPAD)));
        process(*mapper, ARBITRARY_TIME, 0, EV_KEY, KEY_HOME, 0);
        ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(
                WithSource(AINPUT_SOURCE_KEYBOARD | AINPUT_SOURCE_DPAD)));
    }

    // Add a mapper with SOURCE_GAMEPAD
    KeyboardInputMapper& gamepadMapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_GAMEPAD);
    for (auto* mapper : {&keyboardMapper, &dpadMapper, &gamepadMapper}) {
        process(*mapper, ARBITRARY_TIME, 0, EV_KEY, KEY_HOME, 1);
        ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(
                WithSource(AINPUT_SOURCE_KEYBOARD | AINPUT_SOURCE_DPAD | AINPUT_SOURCE_GAMEPAD)));
        process(*mapper, ARBITRARY_TIME, 0, EV_KEY, KEY_HOME, 0);
        ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(
                WithSource(AINPUT_SOURCE_KEYBOARD | AINPUT_SOURCE_DPAD | AINPUT_SOURCE_GAMEPAD)));
    }
}

// --- KeyboardInputMapperTest_ExternalAlphabeticDevice ---

class KeyboardInputMapperTest_ExternalAlphabeticDevice : public InputMapperTest {
protected:
    void SetUp() override {
        InputMapperTest::SetUp(DEVICE_CLASSES | InputDeviceClass::KEYBOARD |
                               InputDeviceClass::ALPHAKEY | InputDeviceClass::EXTERNAL);
    }
};

// --- KeyboardInputMapperTest_ExternalNonAlphabeticDevice ---

class KeyboardInputMapperTest_ExternalNonAlphabeticDevice : public InputMapperTest {
protected:
    void SetUp() override {
        InputMapperTest::SetUp(DEVICE_CLASSES | InputDeviceClass::KEYBOARD |
                               InputDeviceClass::EXTERNAL);
    }
};

TEST_F(KeyboardInputMapperTest_ExternalAlphabeticDevice, WakeBehavior_AlphabeticKeyboard) {
    // For external devices, keys will trigger wake on key down. Media keys should also trigger
    // wake if triggered from external devices.

    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAY, 0, AKEYCODE_MEDIA_PLAY, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAYPAUSE, 0, AKEYCODE_MEDIA_PLAY_PAUSE,
                          POLICY_FLAG_WAKE);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_HOME, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_PLAY, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_PLAY, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_PLAYPAUSE, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_PLAYPAUSE, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
}

TEST_F(KeyboardInputMapperTest_ExternalNonAlphabeticDevice, WakeBehavior_NonAlphabeticKeyboard) {
    // For external devices, keys will trigger wake on key down. Media keys should not trigger
    // wake if triggered from external non-alphaebtic keyboard (e.g. headsets).

    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAY, 0, AKEYCODE_MEDIA_PLAY, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAYPAUSE, 0, AKEYCODE_MEDIA_PLAY_PAUSE,
                          POLICY_FLAG_WAKE);

    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_PLAY, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_PLAY, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_PLAYPAUSE, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_PLAYPAUSE, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
}

TEST_F(KeyboardInputMapperTest_ExternalAlphabeticDevice, DoNotWakeByDefaultBehavior) {
    // Tv Remote key's wake behavior is prescribed by the keylayout file.

    mFakeEventHub->addKey(EVENTHUB_ID, KEY_HOME, 0, AKEYCODE_HOME, POLICY_FLAG_WAKE);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_DOWN, 0, AKEYCODE_DPAD_DOWN, 0);
    mFakeEventHub->addKey(EVENTHUB_ID, KEY_PLAY, 0, AKEYCODE_MEDIA_PLAY, POLICY_FLAG_WAKE);

    addConfigurationProperty("keyboard.doNotWakeByDefault", "1");
    KeyboardInputMapper& mapper =
            constructAndAddMapper<KeyboardInputMapper>(AINPUT_SOURCE_KEYBOARD);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_HOME, 1);
    NotifyKeyArgs args;
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_HOME, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_DOWN, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_DOWN, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(uint32_t(0), args.policyFlags);

    process(mapper, ARBITRARY_TIME, READ_TIME, EV_KEY, KEY_PLAY, 1);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);

    process(mapper, ARBITRARY_TIME + 1, READ_TIME, EV_KEY, KEY_PLAY, 0);
    ASSERT_NO_FATAL_FAILURE(mFakeListener->assertNotifyKeyWasCalled(&args));
    ASSERT_EQ(POLICY_FLAG_WAKE, args.policyFlags);
}

} // namespace android
