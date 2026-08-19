#import "PacketTunnelProvider.h"

// Not part of the default DustX target. Without a project-owned Network
// Extension entitlement this appex cannot be activated for end users.

@implementation DustXPacketTunnelProvider

- (void)startTunnelWithOptions:(NSDictionary<NSString*, NSObject*>*)options
             completionHandler:(void (^)(NSError* _Nullable))completionHandler {
  (void)options;
  NEPacketTunnelNetworkSettings* settings =
      [[NEPacketTunnelNetworkSettings alloc] initWithTunnelRemoteAddress:@"127.0.0.1"];
  settings.IPv4Settings = [[NEIPv4Settings alloc] initWithAddresses:@[ @"100.64.0.1" ]
                                                        subnetMasks:@[ @"255.255.255.0" ]];
  settings.IPv4Settings.includedRoutes = @[ [NEIPv4Route defaultRoute] ];
  settings.MTU = @1280;
  [self setTunnelNetworkSettings:settings completionHandler:^(NSError* error) {
    if (completionHandler) completionHandler(error);
  }];
}

- (void)stopTunnelWithReason:(NEProviderStopReason)reason completionHandler:(void (^)(void))completionHandler {
  (void)reason;
  if (completionHandler) completionHandler();
}

- (void)handleAppMessage:(NSData*)messageData completionHandler:(void (^)(NSData* _Nullable))completionHandler {
  (void)messageData;
  if (completionHandler) completionHandler(nil);
}

@end
