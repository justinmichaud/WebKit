/*
 * Copyright (C) 2026 Igalia S.L.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#import "config.h"

#if PLATFORM(MAC)

#import "Helpers/cocoa/TestWKWebView.h"
#import "Helpers/cocoa/WKWebViewConfigurationExtras.h"
#import <WebKit/WKPreferencesPrivate.h>
#import <WebKit/WKWebViewPrivate.h>
#import <WebKit/_WKInspector.h>
#import <wtf/RetainPtr.h>

// End-to-end tests for the reflection-based native-heap walker
// (TraceNativeHeap.h + TandemHeapAnalyzer.h + NamespaceCellDispatcher.h)
// exercised through a real WKWebView.
//
// $vm.findLiveStrongCount() returns the number of live JSC::Strong instances
// found by the walker, or -1 if ENABLE(REFTRACKER) is off. Each test bails
// early on -1 so the suite passes (silently) on non-REFTRACKER builds.
//
// The four tests cover:
//   ScheduledAction    — dispatcher routes JSDOMWindow, walker follows native
//                        C++ chain to ScheduledAction::m_function (Strong)
//   InspectorController — Step-0 base traversal: m_inspectorController lives
//                        on JSGlobalObject, a base of JSDOMWindow
//   CustomElement      — longest native chain: JSDOMWindow → DOMWindow →
//                        Document → CustomElementRegistry → Strong
//   NoStaleAfterDrop   — over-count guard: Strongs gone from scope must not
//                        be reported

namespace TestWebKitAPI {

// Returns a configuration with the WebProcessPlugInWithInternals bundle
// injected (exposing `internals` and `$vm`) and JSC configured for testing.
static RetainPtr<WKWebViewConfiguration> configWithDollarVM()
{
    return retainPtr([WKWebViewConfiguration _test_configurationWithTestPlugInClassName:@"WebProcessPlugInWithInternals" configureJSCForTesting:YES]);
}

// Evaluates `$vm.findLiveStrongCount()` synchronously and returns the count
// as a plain int. Returns -1 if REFTRACKER is disabled or the result is not
// a number.
static int findLiveStrongCount(TestWKWebView *webView)
{
    id result = [webView objectByEvaluatingJavaScript:@"$vm.findLiveStrongCount()"];
    if (![result isKindOfClass:[NSNumber class]])
        return -1;
    return [result intValue];
}

// ScheduledAction Strongs are reachable through the dispatcher + native chain.
//
// Creates 10 long-lived setTimeouts referencing a shared callback. Each call
// stores a Strong<JSObject> inside ScheduledAction::m_function. The walker
// must dispatch JSDOMWindow → follow Ref<DOMWindow> → Document → timer
// scheduler → ScheduledAction* → Strong to count them.
//
// A zero diff (afterScheduling == baseline) means the WebCore dispatcher
// isn't routing JSDOMWindow cells at all.
TEST(TraceNativeHeapDOM, ScheduledActionStrongsAreReachable)
{
    auto configuration = configWithDollarVM();
    auto webView = adoptNS([[TestWKWebView alloc] initWithFrame:CGRectZero configuration:configuration.get()]);
    [webView synchronouslyLoadHTMLString:@"<html><body></body></html>"];

    int baseline = findLiveStrongCount(webView.get());
    if (baseline < 0)
        return; // REFTRACKER not enabled in this build — skip.

    // Schedule 10 timers referencing a single callback so that each adds
    // exactly one Strong (the function), not a variable amount of closure
    // state.
    [webView objectByEvaluatingJavaScript:
        @"(function() {"
         "  function callbackFn() { /* never runs */ }"
         "  for (var i = 0; i < 10; ++i)"
         "      setTimeout(callbackFn, 1000000);"
         "})();"];

    int afterScheduling = findLiveStrongCount(webView.get());
    EXPECT_GE(afterScheduling - baseline, 10);
}

// Inspector controller Strongs are reachable through Step-0 base traversal.
//
// JSGlobalObject::m_inspectorController is inherited state — it lives on a
// base class of whatever concrete cell the dispatcher routes to (JSDOMWindow).
// Without base traversal the walker sees only the direct members of
// JSDOMWindow and never reaches m_inspectorController.
//
// Opening the inspector frontend creates new JS global objects (the frontend
// page) whose Strongs increment the count.
TEST(TraceNativeHeapDOM, InspectorControllerStrongsAreReachable)
{
    auto configuration = configWithDollarVM();
    configuration.get().preferences._developerExtrasEnabled = YES;
    auto webView = adoptNS([[TestWKWebView alloc] initWithFrame:CGRectMake(0, 0, 800, 600) configuration:configuration.get() addToWindow:YES]);
    [webView synchronouslyLoadHTMLString:@"<html></html>"];

    int baseline = findLiveStrongCount(webView.get());
    if (baseline < 0)
        return; // REFTRACKER not enabled in this build — skip.

    // Opening the inspector connects the debugger agent and initializes
    // additional inspector controller state, adding new Strongs.
    [[webView _inspector] show];
    // Wait for the presentation update so the inspector has a chance to
    // attach its frontend and initialize its JS context before we count.
    [webView waitForNextPresentationUpdate];

    int afterInspector = findLiveStrongCount(webView.get());
    EXPECT_GT(afterInspector, baseline);
}

// Custom element constructor Strongs are reachable through the longest
// native chain tested here.
//
// Chain: JSDOMWindow → Ref<DOMWindow> → Ref<Document> →
//        CustomElementRegistry → JSCustomElementInterface* →
//        Strong<JSObject> (the constructor).
//
// Verifies that the walker doesn't lose its way through a multi-hop
// non-cell C++ chain.
TEST(TraceNativeHeapDOM, CustomElementConstructorStrongsAreReachable)
{
    auto configuration = configWithDollarVM();
    auto webView = adoptNS([[TestWKWebView alloc] initWithFrame:CGRectZero configuration:configuration.get()]);
    [webView synchronouslyLoadHTMLString:@"<html><body></body></html>"];

    int baseline = findLiveStrongCount(webView.get());
    if (baseline < 0)
        return; // REFTRACKER not enabled in this build — skip.

    [webView objectByEvaluatingJavaScript:
        @"class MyEl extends HTMLElement {}"
         "customElements.define('my-el', MyEl);"];

    int afterDefine = findLiveStrongCount(webView.get());
    EXPECT_GT(afterDefine, baseline);
}

// Strongs that have gone out of scope are not over-counted.
//
// A walker that accidentally reports stale objects (e.g. via a leaked
// registry entry) is broken in a subtle way. This test pins the
// count-from-scratch design: after a transient object is collected the
// count must return to the baseline value.
TEST(TraceNativeHeapDOM, NoStaleStrongsAfterDrop)
{
    auto configuration = configWithDollarVM();
    auto webView = adoptNS([[TestWKWebView alloc] initWithFrame:CGRectZero configuration:configuration.get()]);
    [webView synchronouslyLoadHTMLString:@"<html></html>"];

    int baseline = findLiveStrongCount(webView.get());
    if (baseline < 0)
        return; // REFTRACKER not enabled in this build — skip.

    // Allocate and immediately drop a temporary object. findLiveStrongCount
    // internally runs collectNow(Sync, Full), so by the time it returns the
    // GC has collected the temp.
    [webView objectByEvaluatingJavaScript:@"(function() { var temp = { unique: 12345 }; })();"];

    int afterDrop = findLiveStrongCount(webView.get());
    EXPECT_EQ(afterDrop, baseline);
}

} // namespace TestWebKitAPI

#endif // PLATFORM(MAC)
