#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "app.hpp"
#include "util.hpp"

#include <string>

@interface DustXApp : NSObject <NSApplicationDelegate, WKNavigationDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) WKWebView* web;
@property(nonatomic, strong) NSButton* remoteBtn;
@property(nonatomic, strong) NSButton* camBtn;
@property(nonatomic, strong) NSButton* meshBtn;
@property(nonatomic, assign) int port;
@end

@implementation DustXApp

- (void)applicationDidFinishLaunching:(NSNotification*)note {
  (void)note;
  NSRect rect = NSMakeRect(120, 80, 1180, 780);
  self.window = [[NSWindow alloc]
      initWithContentRect:rect
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  self.window.title = @"尘埃X";
  self.window.minSize = NSMakeSize(800, 560);

  NSView* root = self.window.contentView;
  NSView* bar = [[NSView alloc] initWithFrame:NSMakeRect(0, rect.size.height - 52, rect.size.width, 52)];
  bar.wantsLayer = YES;
  bar.layer.backgroundColor = [[NSColor colorWithCalibratedRed:0.06 green:0.11 blue:0.18 alpha:1] CGColor];
  bar.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
  [root addSubview:bar];

  self.remoteBtn = [self makeTab:@"远程控制" tag:1];
  self.camBtn = [self makeTab:@"手机摄像头" tag:2];
  self.meshBtn = [self makeTab:@"跨网互访" tag:3];
  self.remoteBtn.frame = NSMakeRect(16, 10, 120, 32);
  self.camBtn.frame = NSMakeRect(146, 10, 120, 32);
  self.meshBtn.frame = NSMakeRect(276, 10, 120, 32);
  [bar addSubview:self.remoteBtn];
  [bar addSubview:self.camBtn];
  [bar addSubview:self.meshBtn];

  WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
  cfg.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
  if (@available(macOS 10.13, *)) {
    cfg.websiteDataStore = [WKWebsiteDataStore defaultDataStore];
  }
  NSRect webRect = NSMakeRect(0, 0, rect.size.width, rect.size.height - 52);
  self.web = [[WKWebView alloc] initWithFrame:webRect configuration:cfg];
  self.web.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
  self.web.navigationDelegate = self;
  [root addSubview:self.web];

  NSMenu* menubar = [NSMenu new];
  NSMenuItem* appItem = [NSMenuItem new];
  [menubar addItem:appItem];
  NSMenu* appMenu = [NSMenu new];
  [appMenu addItemWithTitle:@"退出尘埃X" action:@selector(terminate:) keyEquivalent:@"q"];
  appItem.submenu = appMenu;
  NSApp.mainMenu = menubar;

  [self selectTab:2];
  [self.window center];
  [self.window makeKeyAndOrderFront:nil];
  [NSApp activateIgnoringOtherApps:YES];
}

- (NSButton*)makeTab:(NSString*)title tag:(NSInteger)tag {
  NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(0, 0, 120, 32)];
  b.title = title;
  b.tag = tag;
  b.target = self;
  b.action = @selector(tabClicked:);
  b.bordered = NO;
  b.wantsLayer = YES;
  b.layer.cornerRadius = 8;
  b.layer.masksToBounds = YES;
  return b;
}

- (void)styleTab:(NSButton*)button selected:(BOOL)selected {
  NSColor* bg = selected ? [NSColor colorWithCalibratedRed:0.18 green:0.50 blue:0.93 alpha:1]
                         : [NSColor colorWithCalibratedRed:1 green:1 blue:1 alpha:0.16];
  button.layer.backgroundColor = bg.CGColor;
  NSDictionary* attrs = @{
    NSForegroundColorAttributeName : [NSColor whiteColor],
    NSFontAttributeName : [NSFont systemFontOfSize:13 weight:selected ? NSFontWeightSemibold : NSFontWeightMedium],
  };
  button.attributedTitle = [[NSAttributedString alloc] initWithString:button.title ?: @"" attributes:attrs];
}

- (void)tabClicked:(NSButton*)sender {
  [self selectTab:sender.tag];
}

- (void)selectTab:(NSInteger)tag {
  [self styleTab:self.remoteBtn selected:tag == 1];
  [self styleTab:self.camBtn selected:tag == 2];
  [self styleTab:self.meshBtn selected:tag == 3];
  if (tag == 2) {
    NSString* url = [NSString stringWithFormat:@"http://127.0.0.1:%d/cam.html", self.port];
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
  } else if (tag == 3) {
    NSString* url = [NSString stringWithFormat:@"http://127.0.0.1:%d/mesh.html", self.port];
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
  } else {
    std::string remote = dustx::remote_console_url();
    NSString* url = [NSString stringWithUTF8String:remote.c_str()];
    [self.web loadRequest:[NSURLRequest requestWithURL:[NSURL URLWithString:url]]];
  }
}

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
  (void)sender;
  return YES;
}

@end

namespace dustx {

int run_native_app(int port) {
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    DustXApp* app = [DustXApp new];
    app.port = port;
    NSApp.delegate = app;
    [NSApp run];
  }
  return 0;
}

}  // namespace dustx
