// ============================================================================
//  AOTDroid-Adaptive-iOS  |  ios/AppShell.mm
//  UIKit launcher: hardware profiler readout + boot-selector UI + Metal host.
// ============================================================================
#import <UIKit/UIKit.h>
#include "../src/profiler/hardware_profiler.h"
#include "../src/runtime/container_boot.h"
#include "../src/gfx/hwc_metal_bridge.h"
#include "../src/jetsam/jetsam_guard.h"

@interface AOTDroidAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow* window;
@end

@interface AOTDroidViewController : UIViewController
@end

@implementation AOTDroidAppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)opts {
  (void)application; (void)opts;
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [[AOTDroidViewController alloc] init];
  [self.window makeKeyAndVisible];
  return YES;
}
@end

@implementation AOTDroidViewController {
  UITextView* _log;
  NSDictionary* _profile;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithRed:0.05 green:0.07 blue:0.10 alpha:1];

  NSDictionary* probe = [[AOTDroidProfiler shared] probe];
  _profile = [[AOTDroidProfiler shared] selectProfile];

  UILabel* title = [[UILabel alloc] init];
  title.text = @"AOTDroid";
  title.font = [UIFont systemFontOfSize:34 weight:UIFontWeightBold];
  title.textColor = UIColor.whiteColor;
  [title sizeToFit];
  title.frame = CGRectMake(20, 70, title.bounds.size.width, title.bounds.size.height);
  [self.view addSubview:title];

  _log = [[UITextView alloc] initWithFrame:
      CGRectMake(20, 120, self.view.bounds.size.width - 40,
                 self.view.bounds.size.height - 320)];
  _log.editable = NO;
  _log.textColor = [UIColor colorWithRed:0.8 green:0.9 blue:0.8 alpha:1];
  _log.font = [UIFont fontWithName:@"Menlo" size:12];
  _log.backgroundColor = [UIColor colorWithWhite:0 alpha:0.4];
  _log.layer.cornerRadius = 12;
  [self.view addSubview:_log];

  NSMutableString* s = [NSMutableString string];
  [s appendFormat:@"SoC        : %@ (%@)\n", probe[@"model"], probe[@"cpuFamilyName"]];
  [s appendFormat:@"RAM        : %.0f MB  available %.0f MB\n",
      ((NSNumber*)probe[@"memSize"]).doubleValue / 1048576.0,
      ((NSNumber*)probe[@"procAvailable"]).doubleValue / 1048576.0];
  [s appendFormat:@"Page size  : %@\n", probe[@"pageSize"]];
  [s appendFormat:@"Cores      : %@\n", probe[@"cores"]];
  [s appendFormat:@"Tier       : %@\n", ((NSNumber*)probe[@"tier"]).intValue == 2
      ? @"Tier 2 (A14/M1+ -> Android 10)" : @"Tier 1 (A13- -> Android 7.1/8.1)"];
  [s appendFormat:@"Selection  : %@\n", _profile[@"displayName"]];
  [s appendFormat:@"Profile    : %@ (API %@)\n", _profile[@"profileTag"],
      _profile[@"apiLevel"]];
  [s appendFormat:@"Rootfs     : %@\n", _profile[@"rootfsImage"]];
  [s appendFormat:@"ART heap   : %@\n", _profile[@"artHeapOpts"]];
  [s appendFormat:@"Framebuffer: %@x%@\n", _profile[@"fbW"], _profile[@"fbH"]];
  _log.text = s;

  NSArray* options = @[@"auto", @"android-7.1", @"android-8.1", @"android-10"];
  for (NSUInteger i = 0; i < options.count; ++i) {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    b.tag = 100 + (NSInteger)i;
    b.frame = CGRectMake(20 + (CGFloat)i * ((self.view.bounds.size.width - 50) / 4) + 4,
                         self.view.bounds.size.height - 170,
                         (self.view.bounds.size.width - 50) / 4 - 8, 56);
    b.backgroundColor = [UIColor colorWithWhite:0.25 alpha:1];
    b.layer.cornerRadius = 12;
    b.tintColor = UIColor.whiteColor;
    b.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
    [b setTitle:options[i] forState:UIControlStateNormal];
    [b addTarget:self action:@selector(bootOption:) forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:b];
  }

  UIButton* boot = [UIButton buttonWithType:UIButtonTypeSystem];
  boot.frame = CGRectMake(20, self.view.bounds.size.height - 100,
                          self.view.bounds.size.width - 40, 64);
  boot.backgroundColor = [UIColor colorWithRed:0.1 green:0.55 blue:0.25 alpha:1];
  boot.layer.cornerRadius = 14;
  boot.tintColor = UIColor.whiteColor;
  boot.titleLabel.font = [UIFont systemFontOfSize:20 weight:UIFontWeightBold];
  [boot setTitle:@"Boot Container" forState:UIControlStateNormal];
  [boot addTarget:self action:@selector(bootNow:) forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:boot];
}

- (void)bootOption:(UIButton*)sender {
  NSArray* options = @[@"auto", @"android-7.1", @"android-8.1", @"android-10"];
  NSString* choice = options[sender.tag - 100];
  [[NSUserDefaults standardUserDefaults] setObject:choice
                                            forKey:@"AOTDroidBootOverride"];
  _profile = [[AOTDroidProfiler shared] selectProfile];
  NSMutableString* s = [_log.text mutableCopy];
  [s appendFormat:@"\n[override] %@ -> %@ (%@ MB budget)", choice,
      _profile[@"profileTag"], _profile[@"rssBudgetMB"]];
  _log.text = s;
}

- (void)bootNow:(UIButton*)sender {
  (void)sender;
  // 1. Jetsam guard first: never allocate before the pressure hooks exist.
  aotdroid::jetsamGuardInstall(nil);

  // 2. Container boot: rootfs pack + binder domains + properties + init.
  aotdroid::BootConfig cfg;
  cfg.apiLevel = ((NSNumber*)_profile[@"apiLevel"]).unsignedIntValue;
  cfg.profileTag = [_profile[@"profileTag"] UTF8String];
  cfg.containerRoot =
      [NSHomeDirectory() UTF8String];   // app sandbox container
  aotdroid::BootResult r = aotdroid::containerBoot(cfg);
  NSMutableString* s = [_log.text mutableCopy];
  if (r.ok) {
    [s appendFormat:@"\n[boot] container up (init domain %u, %zu property bytes)",
        r.initDomain, r.propertyBytes];
    // 3. Metal presentation host at the profiled framebuffer geometry.
    aotdroid::hwcMetalAttachToView((__bridge void*)self,
                         ((NSNumber*)_profile[@"fbW"]).unsignedIntValue,
                         ((NSNumber*)_profile[@"fbH"]).unsignedIntValue,
                         96 * 1024 * 1024);
  } else {
    [s appendFormat:@"\n[boot] FAILED: %s", r.error.c_str()];
  }
  _log.text = s;
}

@end
