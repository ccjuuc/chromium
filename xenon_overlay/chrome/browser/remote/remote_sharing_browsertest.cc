// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/test/browser_test.h"
#include "content/public/test/browser_test_utils.h"
#include "content/public/test/test_navigation_observer.h"
#include "net/http/http_status_code.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_request.h"
#include "net/test/embedded_test_server/http_response.h"
#include "url/gurl.h"

namespace xenon {
namespace {

constexpr char kTargetTitle[] = "Xenon Remote browser test capture target";
constexpr char kHostUrl[] = "chrome://xenon-remote/";

std::unique_ptr<net::test_server::HttpResponse> HandleTargetRequest(
    const net::test_server::HttpRequest& request) {
  if (request.relative_url != "/remote-sharing-target") {
    return nullptr;
  }
  auto response = std::make_unique<net::test_server::BasicHttpResponse>();
  response->set_code(net::HTTP_OK);
  response->set_content_type("text/html");
  response->set_content(R"HTML(
    <!doctype html>
    <meta charset="utf-8">
    <title>Xenon Remote browser test capture target</title>
    <style>body { background: #268050; } input { font-size: 30px; }</style>
    <input id="remote-input" aria-label="Remote input target">
    <canvas id="animation" width="160" height="90"></canvas>
    <script>
      const context = document.getElementById('animation').getContext('2d');
      let frame = 0;
      setInterval(() => {
        context.fillStyle = ++frame % 2 ? '#c02040' : '#2040c0';
        context.fillRect(0, 0, 160, 90);
      }, 50);
    </script>
  )HTML");
  return response;
}

class XenonRemoteSharingBrowserTest : public InProcessBrowserTest {
 public:
  void SetUpCommandLine(base::CommandLine* command_line) override {
    InProcessBrowserTest::SetUpCommandLine(command_line);
    // This picker automation can select only our test tab. The tests never
    // select a real desktop/window or inject operating-system input.
    command_line->AppendSwitchASCII(
        switches::kAutoSelectTabCaptureSourceByTitle, kTargetTitle);
  }

  void SetUpOnMainThread() override {
    InProcessBrowserTest::SetUpOnMainThread();
    embedded_test_server()->RegisterRequestHandler(
        base::BindRepeating(&HandleTargetRequest));
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  void TearDownOnMainThread() override {
    if (host_ && host_->GetLastCommittedURL() == GURL(kHostUrl)) {
      EXPECT_TRUE(content::ExecJs(host_, R"JS(
        (async () => {
          const state = globalThis.remoteTest;
          if (!state) return;
          state.host?.close();
          state.viewer?.close();
          for (const track of state.stream?.getTracks() || []) track.stop();
          state.video?.remove();
          await state.sendWithPromise('remoteStop');
        })()
      )JS"));
    }
    host_ = nullptr;
    target_ = nullptr;
    InProcessBrowserTest::TearDownOnMainThread();
  }

 protected:
  void OpenTargetAndHost() {
    ASSERT_TRUE(ui_test_utils::NavigateToURL(
        browser(), embedded_test_server()->GetURL("/remote-sharing-target")));
    target_ = browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_EQ(kTargetTitle, content::EvalJs(target_, "document.title"));

    chrome::AddTabAt(browser(), GURL(kHostUrl), -1, true);
    host_ = browser()->tab_strip_model()->GetActiveWebContents();
    ASSERT_TRUE(content::WaitForLoadStop(host_));
    ASSERT_EQ(GURL(kHostUrl), host_->GetLastCommittedURL());
    ASSERT_TRUE(content::ExecJs(host_, R"JS(
      (async () => {
        const {sendWithPromise} = await import('chrome://resources/js/cr.js');
        await import('./session.js');
        globalThis.remoteTest = {sendWithPromise};
        remoteTest.withTimeout = (promise, label) => new Promise(
          (resolve, reject) => {
            const timer = setTimeout(() => reject(new Error(label)), 15000);
            Promise.resolve(promise).then(value => {
              clearTimeout(timer);
              resolve(value);
            }, error => {
              clearTimeout(timer);
              reject(error);
            });
          });
        remoteTest.capture = async () => {
          remoteTest.source = await sendWithPromise('remoteChooseSource',
              {mode: 'tab', audio: false});
          remoteTest.constraints = {audio: false, video: {mandatory: {
            chromeMediaSource: 'desktop',
            chromeMediaSourceId: remoteTest.source.streamId,
            maxWidth: 640, maxHeight: 480, maxFrameRate: 15,
          }}};
          remoteTest.stream = await navigator.mediaDevices.getUserMedia(
              remoteTest.constraints);
          return remoteTest.source.mode === 'tab' &&
              remoteTest.source.generation > 0 &&
              remoteTest.stream.getVideoTracks().length === 1 &&
              remoteTest.stream.getVideoTracks()[0].readyState === 'live';
        };
      })()
    )JS"));
  }

  raw_ptr<content::WebContents> target_ = nullptr;
  raw_ptr<content::WebContents> host_ = nullptr;
};

IN_PROC_BROWSER_TEST_F(XenonRemoteSharingBrowserTest,
                       TabCaptureReachesWebRtcReceiver) {
  OpenTargetAndHost();
  ASSERT_TRUE(host_);
  ASSERT_EQ(true, content::EvalJs(host_, "remoteTest.capture()"));

  // Use the shipped pairing/session code with two real peer connections, not
  // fake transports. No STUN/TURN service or external network is required.
  EXPECT_EQ(true, content::EvalJs(host_, R"JS(
    (async () => {
      const state = remoteTest;
      state.video = document.createElement('video');
      state.video.muted = true;
      state.video.playsInline = true;
      state.video.style.cssText =
          'position:fixed;top:0;left:0;width:320px;height:240px;z-index:10000';
      document.body.append(state.video);
      state.host = new XenonRemote.RemoteSession('host');
      state.viewer = new XenonRemote.RemoteSession('viewer', {
        onStream(stream) {
          // Audio and video track events can report the same MediaStream.
          // Reassigning it resets playback and cancels pending frame callbacks.
          if (state.video.srcObject !== stream) state.video.srcObject = stream;
        },
      });
      const offer = await state.host.createOffer(state.stream);
      const answer = await state.viewer.createAnswer(offer);
      await state.host.acceptAnswer(answer);
      await state.withTimeout(state.video.play(),
          'Remote video playback did not start');
      // A browser-test window can be occluded: presentation callbacks then
      // stop even when RTP video is decoded. Verify actual decoded frames and
      // pixels from the green capture fixture without depending on visibility.
      const canvas = document.createElement('canvas');
      canvas.width = canvas.height = 1;
      const context = canvas.getContext('2d', {willReadFrequently: true});
      const deadline = performance.now() + 15000;
      let inbound;
      let pixel;
      do {
        const stats = await state.viewer.peer.getStats();
        inbound = [...stats.values()].find(item =>
            item.type === 'inbound-rtp' && item.kind === 'video');
        if (inbound?.framesDecoded > 0 && state.video.readyState >= 2 &&
            state.video.videoWidth > 0 && state.video.videoHeight > 0) {
          context.drawImage(state.video, state.video.videoWidth / 2,
              state.video.videoHeight / 2, 1, 1, 0, 0, 1, 1);
          pixel = [...context.getImageData(0, 0, 1, 1).data];
          if (pixel[1] > pixel[0] + 40 && pixel[1] > pixel[2] + 20) {
            return state.host.peer.connectionState === 'connected' &&
                state.viewer.peer.connectionState === 'connected';
          }
        }
        await new Promise(resolve => setTimeout(resolve, 50));
      } while (performance.now() < deadline);
      throw new Error('No decoded capture pixels: ' + JSON.stringify({
        inbound, pixel, visibility: document.visibilityState,
        readyState: state.video.readyState,
        width: state.video.videoWidth, height: state.video.videoHeight,
      }));
    })()
  )JS"));
}

IN_PROC_BROWSER_TEST_F(XenonRemoteSharingBrowserTest,
                       CaptureTokenIsSingleUseAndPageControlIsRevocable) {
  OpenTargetAndHost();
  ASSERT_TRUE(host_);
  ASSERT_TRUE(target_);
  ASSERT_EQ(true, content::EvalJs(host_, "remoteTest.capture()"));

  EXPECT_EQ(true, content::EvalJs(host_, R"JS(
    (async () => {
      try {
        const duplicate = await navigator.mediaDevices.getUserMedia(
            remoteTest.constraints);
        duplicate.getTracks().forEach(track => track.stop());
        return false;
      } catch {
        return true;
      }
    })()
  )JS"));

  ASSERT_TRUE(content::ExecJs(target_, R"JS(
    (() => {
      const input = document.getElementById('remote-input');
      input.focus();
      globalThis.remoteInputResult = new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(
            'Remote page input did not arrive')), 15000);
        input.addEventListener('input', () => {
          clearTimeout(timer);
          resolve(input.value);
        }, {once: true});
      });
    })()
  )JS"));

  ASSERT_EQ(true, content::EvalJs(host_, R"JS(
    (async () => {
      const state = remoteTest;
      const result = await state.sendWithPromise('remoteSetControl', {
        generation: state.source.generation, enabled: true,
      });
      if (!result.enabled) return false;
      chrome.send('remoteInput', [{generation: state.source.generation,
        type: 'text', text: 'remote-input-ok'}]);
      return true;
    })()
  )JS"));
  ASSERT_EQ("remote-input-ok", content::EvalJs(target_, "remoteInputResult"));

  // Stop invalidates the generation in the browser process. Both a delayed
  // input packet and an attempt to re-enable the old target must be rejected.
  EXPECT_EQ(true, content::EvalJs(host_, R"JS(
    (async () => {
      const state = remoteTest;
      await state.sendWithPromise('remoteStop');
      chrome.send('remoteInput', [{generation: state.source.generation,
        type: 'text', text: '-must-not-arrive'}]);
      try {
        await state.sendWithPromise('remoteSetControl', {
          generation: state.source.generation, enabled: true,
        });
        return false;
      } catch {
        return true;
      }
    })()
  )JS"));
  EXPECT_EQ("remote-input-ok",
            content::EvalJs(target_,
                            "document.getElementById('remote-input').value"));
}

IN_PROC_BROWSER_TEST_F(XenonRemoteSharingBrowserTest,
                       InvalidModesAndViewerCannotAcquireHostControl) {
  OpenTargetAndHost();
  ASSERT_TRUE(host_);
  EXPECT_EQ(true, content::EvalJs(host_, R"JS(
    (async () => {
      for (const options of [{mode: 'window:1'}, {mode: 'screen:0'},
                             {mode: {type: 'window'}}, {audio: true}]) {
        try {
          await remoteTest.sendWithPromise('remoteChooseSource', options);
          return false;
        } catch {}
      }
      try {
        await remoteTest.sendWithPromise('remoteSetControl',
            {generation: 1, enabled: true});
        return false;
      } catch {}
      return true;
    })()
  )JS"));

  const GURL viewer_url("chrome://xenon-remote/viewer.html");
  // Wait for the host module and a round-trip behind its startup capabilities
  // request before deliberately disabling callbacks for the guard check.
  ASSERT_TRUE(content::ExecJs(host_, R"JS(
    (async () => {
      await import('./index.js');
      await remoteTest.sendWithPromise('remoteGetCapabilities');
    })()
  )JS"));
  auto* host_web_ui = host_->GetWebUI();
  ASSERT_TRUE(host_web_ui);
  ASSERT_EQ(1u, host_web_ui->GetHandlersForTesting()->size());
  auto* host_handler = host_web_ui->GetHandlersForTesting()->front().get();
  host_handler->DisallowJavascript();
  ASSERT_FALSE(host_handler->IsJavascriptAllowed());
  // A same-document path change preserves the controller and its handler.
  // Requests must still check the current document before allowing callbacks.
  content::TestNavigationObserver path_change_observer(host_);
  path_change_observer.set_wait_event(
      content::TestNavigationObserver::WaitEvent::kNavigationFinished);
  ASSERT_TRUE(content::ExecJs(
      host_, "history.replaceState(null, '', '/viewer.html');"));
  path_change_observer.Wait();
  ASSERT_EQ(viewer_url, host_->GetLastCommittedURL());
  ASSERT_EQ(host_web_ui, host_->GetWebUI());
  base::ListValue capability_args;
  capability_args.Append("blocked-viewer-capability-request");
  host_web_ui->ProcessWebUIMessage(viewer_url, "remoteGetCapabilities",
                                   std::move(capability_args));
  EXPECT_FALSE(host_handler->IsJavascriptAllowed());

  // A fresh viewer document must never install the host's native API. Opening
  // a separate WebContents avoids depending on same-WebUI navigation reuse.
  chrome::AddTabAt(browser(), viewer_url, -1, true);
  auto* viewer = browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_NE(host_, viewer);
  ASSERT_TRUE(content::WaitForLoadStop(viewer));
  ASSERT_EQ(viewer_url, viewer->GetLastCommittedURL());
  ASSERT_TRUE(viewer->GetWebUI());
  // Inspect the registered handlers directly; sending an unhandled promise
  // request would hang and would not meaningfully verify the separation.
  EXPECT_TRUE(viewer->GetWebUI()->GetHandlersForTesting()->empty());
  EXPECT_EQ("function",
            content::EvalJs(viewer, "typeof XenonRemote.RemoteSession"));
}

}  // namespace
}  // namespace xenon
