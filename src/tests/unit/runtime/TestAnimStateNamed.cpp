//===----------------------------------------------------------------------===//
//
// Part of the Zanna project, under the GNU GPL v3.
// See LICENSE for license information.
//
//===----------------------------------------------------------------------===//
//
// File: src/tests/unit/runtime/TestAnimStateNamed.cpp
// Purpose: Tests for AnimStateMachine named state API and frame events.
//
//===----------------------------------------------------------------------===//

#include "tests/TestHarness.hpp"

#include <cstring>

extern "C" {
#include "rt_string.h"
void *rt_animstate_new(void);
void rt_animstate_add_state(
    void *asm_, int64_t state_id, int64_t start, int64_t end, int64_t dur, int8_t loop);
void rt_animstate_add_named(
    void *asm_, void *name, int64_t start, int64_t end, int64_t dur, int8_t loop);
void rt_animstate_play(void *asm_, void *name);
void rt_animstate_update(void *asm_);
void rt_animstate_clear_flags(void *asm_);
rt_string rt_animstate_current_name(void *asm_);
int64_t rt_animstate_current_frame(void *asm_);
int8_t rt_animstate_just_entered(void *asm_);
void rt_animstate_set_event_frame(void *asm_, int64_t frame);
int8_t rt_animstate_event_fired(void *asm_);
int8_t rt_animstate_set_initial(void *asm_, int64_t id);
int8_t rt_animstate_add_event(void *asm_, int64_t state_id, int64_t frame, int64_t event_id);
rt_string rt_const_cstr(const char *s);
const char *rt_string_cstr(rt_string s);
}

TEST(AnimStateNamed, AddAndPlayByName) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("walk"), 0, 3, 8, 1);
    rt_animstate_set_initial(sm, 0); // ID 0 = "walk"
    rt_animstate_play(sm, (void *)rt_const_cstr("walk"));
    EXPECT_EQ(rt_animstate_current_frame(sm), 0);
}

TEST(AnimStateNamed, TransitionByName) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("idle"), 0, 3, 8, 1);
    rt_animstate_add_named(sm, (void *)rt_const_cstr("run"), 4, 7, 6, 1);
    rt_animstate_set_initial(sm, 0);
    rt_animstate_play(sm, (void *)rt_const_cstr("run"));
    EXPECT_TRUE(rt_animstate_just_entered(sm));
    EXPECT_EQ(rt_animstate_current_frame(sm), 4); // run starts at frame 4
}

TEST(AnimStateNamed, GetStateName) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("jump"), 8, 10, 4, 0);
    rt_animstate_set_initial(sm, 0);
    rt_string name = rt_animstate_current_name(sm);
    ASSERT_TRUE(name != nullptr);
    const char *cname = rt_string_cstr(name);
    EXPECT_EQ(strcmp(cname, "jump"), 0);
}

TEST(AnimStateNamed, UnknownNameNoOp) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("idle"), 0, 3, 8, 1);
    rt_animstate_set_initial(sm, 0);
    rt_animstate_clear_flags(sm);
    rt_animstate_play(sm, (void *)rt_const_cstr("nonexistent"));
    // Should not crash, state unchanged
    EXPECT_EQ(rt_animstate_current_frame(sm), 0);
}

TEST(AnimStateNamed, EventFrameFires) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("attack"), 0, 5, 1, 0);
    rt_animstate_set_initial(sm, 0);
    rt_animstate_set_event_frame(sm, 3);

    // Advance to frame 3
    for (int i = 0; i < 3; i++)
        rt_animstate_update(sm);

    EXPECT_TRUE(rt_animstate_event_fired(sm));
}

TEST(AnimStateNamed, EventAutoClears) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("attack"), 0, 5, 1, 0);
    rt_animstate_set_initial(sm, 0);
    rt_animstate_set_event_frame(sm, 1);

    rt_animstate_update(sm);                    // frame 0 → 1
    EXPECT_TRUE(rt_animstate_event_fired(sm));  // first check: true
    EXPECT_FALSE(rt_animstate_event_fired(sm)); // second check: auto-cleared
}

TEST(AnimStateNamed, EventDoesNotRetriggerWhileFrameIsUnchanged) {
    void *sm = rt_animstate_new();
    rt_animstate_add_named(sm, (void *)rt_const_cstr("hold"), 0, 1, 4, 0);
    rt_animstate_set_initial(sm, 0);
    rt_animstate_set_event_frame(sm, 1);

    for (int i = 0; i < 4; ++i)
        rt_animstate_update(sm);
    EXPECT_TRUE(rt_animstate_event_fired(sm));

    rt_animstate_update(sm);
    rt_animstate_update(sm);
    EXPECT_FALSE(rt_animstate_event_fired(sm));
}

TEST(AnimStateNamed, RejectsEventFramesOutsideClip) {
    void *sm = rt_animstate_new();
    rt_animstate_add_state(sm, 0, 2, 4, 1, 1);
    rt_animstate_add_state(sm, 1, 6, 3, 1, 1);

    EXPECT_TRUE(rt_animstate_add_event(sm, 0, 2, 20));
    EXPECT_TRUE(rt_animstate_add_event(sm, 0, 4, 40));
    EXPECT_FALSE(rt_animstate_add_event(sm, 0, 1, 10));
    EXPECT_FALSE(rt_animstate_add_event(sm, 0, 5, 50));

    EXPECT_TRUE(rt_animstate_add_event(sm, 1, 6, 60));
    EXPECT_TRUE(rt_animstate_add_event(sm, 1, 3, 30));
    EXPECT_FALSE(rt_animstate_add_event(sm, 1, 2, 20));
    EXPECT_FALSE(rt_animstate_add_event(sm, 1, 7, 70));
}

// VDOC-274: AddNamed must allocate a genuinely unused state ID and store the name
// at the actual clip index. Here a numeric state already owns id == clip_count (1),
// which the old code would have overwritten while stranding the name in an
// out-of-range slot, leaving Play("walk") a no-op and corrupting the numeric clip.
TEST(AnimStateNamed, AddNamedDoesNotClobberNumericState) {
    void *sm = rt_animstate_new();
    rt_animstate_add_state(sm, 1, 0, 5, 1, 1); // clip_count == 1; the only clip owns id 1
    rt_animstate_add_named(sm, (void *)rt_const_cstr("walk"), 10, 12, 1, 0);

    // The name is playable and lands on the named clip's start frame.
    rt_animstate_play(sm, (void *)rt_const_cstr("walk"));
    EXPECT_EQ(rt_animstate_current_frame(sm), 10);

    // The numeric state's clip is intact (frames 0..5), not overwritten by 10..12.
    EXPECT_TRUE(rt_animstate_set_initial(sm, 1));
    EXPECT_EQ(rt_animstate_current_frame(sm), 0);
}

// VDOC-275: redefining the active state via AddState must restart its clip so the
// machine tracks the new range instead of being stranded on a frame outside it.
TEST(AnimStateNamed, RedefiningActiveStateRestartsClip) {
    void *sm = rt_animstate_new();
    rt_animstate_add_state(sm, 0, 0, 5, 1, 1);
    EXPECT_TRUE(rt_animstate_set_initial(sm, 0));
    rt_animstate_update(sm); // frame 0 → 1
    EXPECT_EQ(rt_animstate_current_frame(sm), 1);

    // Redefine the active state with a new, distant range (hot-reload).
    rt_animstate_add_state(sm, 0, 100, 101, 1, 1);
    EXPECT_EQ(rt_animstate_current_frame(sm), 100); // restarted at the new start

    rt_animstate_update(sm); // advances inside the new range, not off into frame 2
    EXPECT_EQ(rt_animstate_current_frame(sm), 101);
}

int main() {
    return zanna_test::run_all_tests();
}
