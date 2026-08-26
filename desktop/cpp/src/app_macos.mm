#import <Cocoa/Cocoa.h>
#import <Network/Network.h>
#import <WebKit/WebKit.h>

#include "app.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "update.hpp"
#include "util.hpp"

#include <string>

@interface DustXApp : NSObject <NSApplicationDelegate, NSWindowDelegate, WKNavigationDelegate, WKUIDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* web;
@property(nonatomic, strong) NSMutableArray<NSWindow*>* extras;
@property(nonatomic, assign) int port;
@property(nonatomic, assign) BOOL allowClose;
@property(nonatomic, assign) BOOL loadAlerted;
@end

static void apply_store_proxy(WKWebsiteDataStore* store, bool use_system) {
  if (@available(macOS 14.0, *)) {
    if (use_system) {
      store.proxyConfigurations = nil;
      return;
    }
    nw_endpoint_t ep = nw_endpoint_create_host("127.0.0.1", "9");
    nw_proxy_config_t cfg = nw_proxy_config_create_http_connect(ep, nil);
    nw_proxy_config_add_match_domain(cfg, "dustx-direct-only.invalid");
    store.proxyConfigurations = @[ cfg ];
  }
}

@implementation DustXApp

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  (void)note;
  NSRect rect = NSMakeRect(80, 60, 1280, 820);
  self.window = [[NSWindow alloc]
      initWithContentRect:rect
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"尘埃X";
  self.window.minSize = NSMakeSize(960, 640);
  self.window.delegate = self;
  self.window.animationBehavior = NSWindowAnimationBehaviorNone;

  WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
  cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
  WKWebsiteDataStore* store = [WKWebsiteDataStore defaultDataStore];
  apply_store_proxy(store, dustx::use_system_proxy());
  cfg.websiteDataStore = store;
  self.web = [[WKWebView alloc] initWithFrame:self.window.contentView.bounds configuration:cfg];
  self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  self.web.navigationDelegate = self;
  self.web.UIDelegate = self;
  self.extras = [NSMutableArray new];
  [self.window.contentView addSubview:self.web];

  NSMenu* menubar = [NSMenu new];
  NSMenuItem* appItem = [NSMenuItem new];
  [menubar addItem:appItem];
  NSMenu* appMenu = [NSMenu new];
  [appMenu addItemWithTitle:@"退出尘埃X" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  NSApp.mainMenu = menubar;

  NSString* url = [NSString stringWithFormat:@"http://127.0.0.1:%d/?v=peek4", self.port];
  NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:[NSURL URLWithString:url]];
  req.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
  [self.web loadRequest:req];
  [self.window center];
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
  dustx::set_proxy_apply([self](bool use_system) {
    apply_store_proxy(self.web.configuration.websiteDataStore, use_system);
    [self.web reload];
  });
}

- (void)confirmCloseThen:(void (^)(void))ok cancel:(void (^)(void))cancel {
  [self.web evaluateJavaScript:@"(function(){try{return window.dustxCloseCheck?window.dustxCloseCheck():{confirm:false};}catch(e){return {confirm:false};}})()"
             completionHandler:^(id result, NSError* err) {
               (void)err;
               BOOL need = NO;
               NSString* what = @"";
               if ([result isKindOfClass:[NSDictionary class]]) {
                 need = [result[@"confirm"] boolValue];
                 id text = result[@"text"];
                 if ([text isKindOfClass:[NSString class]]) what = text;
               }
               if (!need) {
                 if (ok) ok();
                 return;
               }
               NSAlert* alert = [NSAlert new];
               alert.alertStyle = NSAlertStyleWarning;
               alert.messageText = @"确定关闭尘埃X？";
               if (what.length) {
                 alert.informativeText =
                     [NSString stringWithFormat:@"当前还有连接：%@。关闭后这些连接会断开。", what];
               } else {
                 alert.informativeText = @"当前还有连接，关闭后会断开。";
               }
               [alert addButtonWithTitle:@"取消"];
               [alert addButtonWithTitle:@"关闭"];
               if ([alert runModal] == NSAlertSecondButtonReturn) {
                 if (ok) ok();
               } else if (cancel) {
                 cancel();
               }
             }];
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)windowFeatures {
  (void)webView;
  (void)windowFeatures;
  NSRect rect = NSMakeRect(140, 80, 1100, 740);
  NSWindow* extra = [[NSWindow alloc]
      initWithContentRect:rect
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  extra.minSize = NSMakeSize(720, 480);
  extra.delegate = self;
  extra.releasedWhenClosed = NO;
  extra.animationBehavior = NSWindowAnimationBehaviorNone;
  NSString* path = action.request.URL.path ?: @"";
  if ([path containsString:@"files"]) extra.title = @"文件传输";
  else if ([path containsString:@"term"]) extra.title = @"终端";
  else if ([path containsString:@"webcam"] || [path containsString:@"camera"] || [path containsString:@"mode=camera"])
    extra.title = @"摄像头";
  else extra.title = @"尘埃X";
  WKWebView* child = [[WKWebView alloc] initWithFrame:extra.contentView.bounds configuration:configuration];
  child.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  child.navigationDelegate = self;
  child.UIDelegate = self;
  [extra.contentView addSubview:child];
  [self.extras addObject:extra];
  [extra makeKeyAndOrderFront:nil];
  return child;
}

- (void)webView:(WKWebView*)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin
                          initiatedByFrame:(WKFrameInfo*)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:(void (^)(WKPermissionDecision decision))decisionHandler API_AVAILABLE(macos(12.0)) {
  (void)webView;
  (void)origin;
  (void)frame;
  (void)type;
  decisionHandler(WKPermissionDecisionGrant);
}

- (BOOL)windowShouldClose:(NSWindow*)sender {
  if (sender != self.window) return YES;
  if (self.allowClose) return YES;
  [self confirmCloseThen:^{
    self.allowClose = YES;
    [sender close];
  }
                  cancel:nil];
  return NO;
}

- (void)windowWillClose:(NSNotification*)note {
  NSWindow* sender = note.object;
  if (sender && sender != self.window) [self.extras removeObject:sender];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  (void)sender;
  if (self.allowClose) return NSTerminateNow;
  [self confirmCloseThen:^{
    self.allowClose = YES;
    [NSApp replyToApplicationShouldTerminate:YES];
  }
                  cancel:^{
                    [NSApp replyToApplicationShouldTerminate:NO];
                  }];
  return NSTerminateLater;
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

- (void)showLoadError:(NSString*)detail {
  if (self.loadAlerted) return;
  self.loadAlerted = YES;
  dustx::log_error("webview", std::string("页面打开失败 ") + (detail ? detail.UTF8String : ""));
  dustx::alert_error(std::string("无法打开本机界面，窗口会是白屏。\n") + (detail ? detail.UTF8String : "") +
                     "\n\n日志：" + dustx::log_file_path());
}

- (void)webView:(WKWebView*)webView didFailProvisionalNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  [self showLoadError:error.localizedDescription];
}

- (void)webView:(WKWebView*)webView didFailNavigation:(WKNavigation*)navigation withError:(NSError*)error {
  (void)webView;
  (void)navigation;
  [self showLoadError:error.localizedDescription];
}

@end

namespace dustx {

void alert_error(const std::string& text) {
  @autoreleasepool {
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleCritical;
    alert.messageText = @"尘埃X";
    alert.informativeText = [NSString stringWithUTF8String:text.c_str()] ?: @"";
    [alert runModal];
  }
}

int run_native_app(int port) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    DustXApp* app = [DustXApp new];
    app.port = port;
    NSApp.delegate = app;
    dustx::set_update_quit([app] {
      dispatch_async(dispatch_get_main_queue(), ^{
        app.allowClose = YES;
        [NSApp terminate:nil];
      });
    });
    [NSApp run];
  }
  return 0;
}

}  // namespace dustx
