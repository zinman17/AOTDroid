// ============================================================================
//  AOTDroid-Adaptive-iOS  |  ios/AppShell.mm
//  UIKit launcher: hardware profiler readout + ROOTFS MANAGER (pick a GSI
//  .zip / .img via the document picker; it gets converted on-device into a
//  rootfs-<tag>.aotpack) + boot selector + Metal host.
// ============================================================================
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "../src/profiler/hardware_profiler.h"
#include "../src/runtime/container_boot.h"
#include "../src/gfx/hwc_metal_bridge.h"
#include "../src/jetsam/jetsam_guard.h"
#include "../src/rootfs/rootfs_manager.h"

@interface AOTDroidAppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow* window;
@end

@interface AOTDroidViewController : UIViewController <UIDocumentPickerDelegate>
+ (void)appendType:(NSString*)ident toArray:(NSMutableArray<UTType*>*)arr;
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
  UILabel* _title;
  UILabel* _subtitle;
  UIView* _rootfsCard;
  UILabel* _rootfsTitle;
  UILabel* _rootfsDetail;
  UIButton* _addBtn;
  UIButton* _removeBtn;
  UIButton* _bootBtn;
  NSMutableArray<UIButton*>* _optionBtns;
  UIProgressView* _progress;
  UILabel* _progressLabel;
  BOOL _importing;
}

// ---------------------------------------------------------------------------
// UI construction
// ---------------------------------------------------------------------------
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithRed:0.05 green:0.07 blue:0.10 alpha:1];

  NSDictionary* probe = [[AOTDroidProfiler shared] probe];
  _profile = [[AOTDroidProfiler shared] selectProfile];

  UILabel* title = [[UILabel alloc] init];
  title.text = @"AOTDroid";
  title.font = [UIFont systemFontOfSize:30 weight:UIFontWeightBold];
  title.textColor = UIColor.whiteColor;
  [title sizeToFit];
  [self.view addSubview:title];
  _title = title;

  UILabel* sub = [[UILabel alloc] init];
  sub.text = [NSString stringWithFormat:@"%@  -  %@ (API %@)",
      _profile[@"profileTag"], _profile[@"displayName"], _profile[@"apiLevel"]];
  sub.font = [UIFont systemFontOfSize:12 weight:UIFontWeightMedium];
  sub.textColor = [UIColor colorWithWhite:0.7 alpha:1];
  [sub sizeToFit];
  [self.view addSubview:sub];
  _subtitle = sub;

  // --- rootfs status card ---------------------------------------------------
  UIView* card = [[UIView alloc] init];
  card.backgroundColor = [UIColor colorWithWhite:0.12 alpha:1];
  card.layer.cornerRadius = 14;
  [self.view addSubview:card];
  _rootfsCard = card;

  _rootfsTitle = [[UILabel alloc] init];
  _rootfsTitle.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
  _rootfsTitle.textColor = UIColor.whiteColor;
  _rootfsTitle.numberOfLines = 0;
  [card addSubview:_rootfsTitle];

  _rootfsDetail = [[UILabel alloc] init];
  _rootfsDetail.font = [UIFont fontWithName:@"Menlo" size:11];
  _rootfsDetail.textColor = [UIColor colorWithWhite:0.62 alpha:1];
  _rootfsDetail.numberOfLines = 0;
  [card addSubview:_rootfsDetail];

  // --- import progress ------------------------------------------------------
  _progress = [[UIProgressView alloc] initWithProgressViewStyle:UIProgressViewStyleDefault];
  _progress.progressTintColor = [UIColor colorWithRed:0.2 green:0.7 blue:0.4 alpha:1];
  _progress.hidden = YES;
  [self.view addSubview:_progress];

  _progressLabel = [[UILabel alloc] init];
  _progressLabel.font = [UIFont fontWithName:@"Menlo" size:11];
  _progressLabel.textColor = [UIColor colorWithWhite:0.7 alpha:1];
  _progressLabel.hidden = YES;
  [self.view addSubview:_progressLabel];

  // --- buttons --------------------------------------------------------------
  _addBtn = [UIButton buttonWithType:UIButtonTypeSystem];
  _addBtn.backgroundColor = [UIColor colorWithRed:0.13 green:0.35 blue:0.62 alpha:1];
  _addBtn.layer.cornerRadius = 12;
  _addBtn.tintColor = UIColor.whiteColor;
  _addBtn.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
  [_addBtn setTitle:@"+  Add RootFS" forState:UIControlStateNormal];
  [_addBtn addTarget:self action:@selector(addRootfsTapped:)
      forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_addBtn];

  _removeBtn = [UIButton buttonWithType:UIButtonTypeSystem];
  _removeBtn.backgroundColor = [UIColor colorWithWhite:0.2 alpha:1];
  _removeBtn.layer.cornerRadius = 12;
  _removeBtn.tintColor = [UIColor colorWithRed:0.95 green:0.45 blue:0.45 alpha:1];
  _removeBtn.titleLabel.font = [UIFont systemFontOfSize:14 weight:UIFontWeightSemibold];
  [_removeBtn setTitle:@"Remove" forState:UIControlStateNormal];
  [_removeBtn addTarget:self action:@selector(removeRootfsTapped:)
      forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_removeBtn];

  _optionBtns = [NSMutableArray array];
  NSArray* options = @[@"auto", @"7.1", @"8.1", @"10"];
  for (NSUInteger i = 0; i < options.count; ++i) {
    UIButton* b = [UIButton buttonWithType:UIButtonTypeSystem];
    b.tag = 100 + (NSInteger)i;
    b.backgroundColor = [UIColor colorWithWhite:0.22 alpha:1];
    b.layer.cornerRadius = 10;
    b.tintColor = UIColor.whiteColor;
    b.titleLabel.font = [UIFont systemFontOfSize:13 weight:UIFontWeightSemibold];
    [b setTitle:options[i] forState:UIControlStateNormal];
    [b addTarget:self action:@selector(bootOption:)
        forControlEvents:UIControlEventTouchUpInside];
    [self.view addSubview:b];
    [_optionBtns addObject:b];
  }

  _bootBtn = [UIButton buttonWithType:UIButtonTypeSystem];
  _bootBtn.backgroundColor = [UIColor colorWithRed:0.1 green:0.55 blue:0.25 alpha:1];
  _bootBtn.layer.cornerRadius = 14;
  _bootBtn.tintColor = UIColor.whiteColor;
  _bootBtn.titleLabel.font = [UIFont systemFontOfSize:19 weight:UIFontWeightBold];
  [_bootBtn setTitle:@"Boot Container" forState:UIControlStateNormal];
  [_bootBtn addTarget:self action:@selector(bootNow:)
      forControlEvents:UIControlEventTouchUpInside];
  [self.view addSubview:_bootBtn];

  // --- console --------------------------------------------------------------
  _log = [[UITextView alloc] init];
  _log.editable = NO;
  _log.textColor = [UIColor colorWithRed:0.8 green:0.9 blue:0.8 alpha:1];
  _log.font = [UIFont fontWithName:@"Menlo" size:11];
  _log.backgroundColor = [UIColor colorWithWhite:0 alpha:0.4];
  _log.layer.cornerRadius = 12;
  _log.layoutManager.allowsNonContiguousLayout = NO;
  [self.view addSubview:_log];

  NSMutableString* s = [NSMutableString string];
  [s appendFormat:@"SoC        : %@ (%@)\n", probe[@"model"], probe[@"cpuFamilyName"]];
  [s appendFormat:@"RAM        : %.0f MB  available %.0f MB\n",
      ((NSNumber*)probe[@"memSize"]).doubleValue / 1048576.0,
      ((NSNumber*)probe[@"procAvailable"]).doubleValue / 1048576.0];
  [s appendFormat:@"Cores      : %@   tier %@\n", probe[@"cores"],
      ((NSNumber*)_profile[@"tier"]).intValue == 2 ? @"2 (A14+)" : @"1 (A13-)"];
  [s appendFormat:@"Selection  : %@  RSS budget %@ MB\n",
      _profile[@"profileTag"], _profile[@"rssBudgetMB"]];
  [s appendFormat:@"Framebuffer: %@x%@\n", _profile[@"fbW"], _profile[@"fbH"]];
  _log.text = s;

  [self layoutAll];
  [self refreshRootfsCard];
}

- (void)viewWillLayoutSubviews {
  [super viewWillLayoutSubviews];
  [self layoutAll];
}

- (void)layoutAll {
  CGFloat W = self.view.bounds.size.width, H = self.view.bounds.size.height;
  CGFloat x = 16, w = W - 32;

  _title.frame = CGRectMake(x, 54, w, 34);
  _subtitle.frame = CGRectMake(x, 88, w, 16);

  CGFloat y = 112;
  _rootfsCard.frame = CGRectMake(x, y, w, 86);
  _rootfsTitle.frame = CGRectMake(12, 10, w - 24, 20);
  _rootfsDetail.frame = CGRectMake(12, 34, w - 24, 44);

  y += 86 + 10;
  _progress.frame = CGRectMake(x, y, w, 4);
  _progressLabel.frame = CGRectMake(x, y + 8, w, 14);

  y += 30;
  CGFloat half = (w - 10) / 2;
  _addBtn.frame = CGRectMake(x, y, half, 46);
  _removeBtn.frame = CGRectMake(x + half + 10, y, half, 46);

  y += 46 + 10;
  CGFloat q = (w - 30) / 4;
  for (NSUInteger i = 0; i < _optionBtns.count; ++i)
    _optionBtns[i].frame = CGRectMake(x + (CGFloat)i * (q + 10), y, q, 38);

  y += 38 + 10;
  _bootBtn.frame = CGRectMake(x, y, w, 56);

  y += 56 + 10;
  _log.frame = CGRectMake(x, y, w, H - y - 24);
}

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
- (void)logLine:(NSString*)line {
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMutableString* s = [_log.text mutableCopy];
    if (s.length) [s appendString:@"\n"];
    [s appendString:line];
    _log.text = s;
    [_log scrollRangeToVisible:NSMakeRange(_log.text.length, 0)];
  });
}

- (void)refreshRootfsCard {
  aotdroid::RootfsStatus st =
      aotdroid::RootfsManager::get().status([NSHomeDirectory() UTF8String]);
  if (!st.installed) {
    _rootfsTitle.text = @"No rootfs installed";
    _rootfsDetail.text = @"tap '+ Add RootFS' and pick one or more GSI "
                         ".zip (system.img inside) or .img files";
    _bootBtn.alpha = 0.45;
    _bootBtn.enabled = NO;
    _removeBtn.enabled = NO;
  } else {
    _rootfsTitle.text = [NSString stringWithFormat:@"RootFS: %@  (Android %@)",
        [NSString stringWithUTF8String:st.profileTag.c_str()],
        [NSString stringWithUTF8String:st.androidVersion.c_str()]];
    _rootfsDetail.text = [NSString stringWithFormat:
        @"API %d   %llu MB   %u files / %u dirs / %u links\n%@",
        st.apiLevel, (unsigned long long)(st.packBytes / 1048576),
        st.fileCount, st.dirCount, st.symlinkCount,
        [NSString stringWithUTF8String:st.fingerprint.c_str()]];
    _bootBtn.alpha = 1.0;
    _bootBtn.enabled = YES;
    _removeBtn.enabled = YES;
  }
}

// ---------------------------------------------------------------------------
// Add RootFS: document picker (multi-select, copy mode) -> batch import.
//
// Sideloaded-app picker bug fixed here with three cooperating changes:
//  1. CONCRETE UTTypes only. The picker silently ignores physical UTIs such
//     as public.data; with nothing allowed, every file in the Files app is
//     greyed out and untappable -- the classic "can't select files" bug on
//     SideStore/AltStore installs. public.zip-archive covers GSI .zip
//     archives, public.disk-image covers raw/sparse .img system images.
//  2. asCopy:YES. The picker hands us a sandbox-local copy under tmp/
//     instead of an open-in-place security scope. Re-signed sideloaded apps
//     frequently fail the scope grant (entitlements are rewritten at install
//     time); a plain copy sidesteps it entirely. We delete the copy after
//     the import so a 1-2 GB GSI does not eat the container twice.
//  3. allowsMultipleSelection: import every picked GSI in one batch; each
//     becomes its own rootfs-<tag>.aotpack (packs coexist; the boot-override
//     buttons pick which one boots).
// ---------------------------------------------------------------------------
+ (void)appendType:(NSString*)ident toArray:(NSMutableArray<UTType*>*)arr {
  UTType* t = [UTType typeWithIdentifier:ident];
  if (t) [arr addObject:t];
}

- (void)addRootfsTapped:(UIButton*)sender {
  (void)sender;
  if (_importing) return;
  NSMutableArray<UTType*>* types = [NSMutableArray array];
  [AOTDroidViewController appendType:@"public.zip-archive" toArray:types];
  [AOTDroidViewController appendType:@"public.disk-image" toArray:types];
  UIDocumentPickerViewController* picker;
  if (@available(iOS 14.0, *)) {
    picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:types asCopy:YES];
  } else {
    picker = [[UIDocumentPickerViewController alloc]
        initWithDocumentTypes:@[@"public.zip-archive", @"public.disk-image"]
                      inMode:UIDocumentPickerModeImport];
  }
  picker.delegate = self;
  picker.allowsMultipleSelection = YES;
  [self presentViewController:picker animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController*)controller
    didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
  (void)controller;
  if (urls.count == 0) return;
  [self importFiles:urls];
}

- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
  (void)controller;
}

- (void)importFiles:(NSArray<NSURL*>*)urls {
  NSString* home = NSHomeDirectory();
  NSString* tmpRoot = NSTemporaryDirectory();
  uint64_t freeBytes = 0;
  NSDictionary* fsAttrs = [[NSFileManager defaultManager]
      attributesOfFileSystemForPath:home error:nil];
  if (fsAttrs) freeBytes = [fsAttrs[NSFileSystemFreeSize] unsignedLongLongValue];
  [self logLine:[NSString stringWithFormat:
      @"[rootfs] importing %lu file%@ (%llu MB free)",
      (unsigned long)urls.count, urls.count == 1 ? @"" : @"s",
      freeBytes / 1048576]];

  _importing = YES;
  _progress.hidden = NO;
  _progressLabel.hidden = NO;
  _progress.progress = 0;
  _progressLabel.text = @"inspecting...";
  _addBtn.enabled = NO;
  _bootBtn.enabled = NO;
  _removeBtn.enabled = NO;

  __weak AOTDroidViewController* weakSelf = self;
  dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
  dispatch_async(q, ^{
    NSUInteger okCount = 0;
    for (NSUInteger i = 0; i < urls.count; ++i) {
      NSURL* url = urls[i];
      NSString* src = url.path;
      // no-op under asCopy (the copy already lives in our sandbox); only
      // matters for the iOS<14 import-mode fallback
      BOOL scoped = [url startAccessingSecurityScopedResource];
      uint64_t srcBytes = 0;
      NSDictionary* fa = [[NSFileManager defaultManager]
          attributesOfItemAtPath:src error:nil];
      if (fa) srcBytes = [fa[NSFileSize] unsignedLongLongValue];
      [weakSelf logLine:[NSString stringWithFormat:
          @"[rootfs] %lu/%lu %@ (%llu MB)",
          (unsigned long)(i + 1), (unsigned long)urls.count,
          url.lastPathComponent, srcBytes / 1048576]];
      if (freeBytes && srcBytes + (512ULL << 20) > freeBytes) {
        [weakSelf logLine:@"[rootfs] WARNING: low free space; import may fail"];
      }
      std::string err;
      bool ok = aotdroid::RootfsManager::get().importTo(
          [src UTF8String], [home UTF8String],
          [weakSelf, i, urls](const aotdroid::ImportProgress& p) {
            dispatch_async(dispatch_get_main_queue(), ^{
              AOTDroidViewController* s = weakSelf;
              if (!s) return;
              s->_progress.progress =
                  ((float)i + p.fraction) / (float)urls.count;
              s->_progressLabel.text = [NSString stringWithFormat:
                  @"[%s] %.0f%%  (%llu / %llu MB)",
                  p.phase.c_str(), p.fraction * 100.0,
                  (unsigned long long)(p.bytesDone / 1048576),
                  (unsigned long long)(p.bytesTotal / 1048576)];
            });
          }, &err);
      if (scoped) [url stopAccessingSecurityScopedResource];
      // Reclaim the picker's tmp/ copy right away (asCopy mode). Only ever
      // touch paths inside our own NSTemporaryDirectory().
      if ([src hasPrefix:tmpRoot])
        [[NSFileManager defaultManager] removeItemAtPath:src error:nil];
      if (ok) {
        ++okCount;
        aotdroid::RootfsStatus st =
            aotdroid::RootfsManager::get().status([home UTF8String]);
        NSString* loc = [NSString stringWithUTF8String:st.packPath.c_str()];
        loc = [loc stringByReplacingOccurrencesOfString:home withString:@"~"];
        [weakSelf logLine:[NSString stringWithFormat:
            @"[rootfs] -> installed %@ (Android %@, API %d): %u files, "
            @"%llu MB at %@",
            [NSString stringWithUTF8String:st.profileTag.c_str()],
            [NSString stringWithUTF8String:st.androidVersion.c_str()],
            st.apiLevel, st.fileCount,
            (unsigned long long)(st.packBytes / 1048576), loc]];
      } else {
        [weakSelf logLine:[NSString stringWithFormat:
            @"[rootfs] -> FAILED: %s", err.c_str()]];
      }
    }
    // summary: list every installed pack for the boot selector
    NSMutableArray<NSString*>* tags = [NSMutableArray array];
    for (const std::string& t :
        aotdroid::RootfsManager::installedTags([home UTF8String]))
      [tags addObject:[NSString stringWithUTF8String:t.c_str()]];
    dispatch_async(dispatch_get_main_queue(), ^{
      AOTDroidViewController* s = weakSelf;
      if (!s) return;
      s->_importing = NO;
      s->_progress.hidden = YES;
      s->_progressLabel.hidden = YES;
      s->_addBtn.enabled = YES;
      [s logLine:[NSString stringWithFormat:
          @"[rootfs] batch done: %lu/%lu ok; packs installed: %@",
          (unsigned long)okCount, (unsigned long)urls.count,
          tags.count ? [tags componentsJoinedByString:@", "] : @"none"]];
      [s refreshRootfsCard];
    });
  });
}

- (void)removeRootfsTapped:(UIButton*)sender {
  (void)sender;
  std::string err;
  bool had = aotdroid::RootfsManager::get().remove([NSHomeDirectory() UTF8String],
                                                   &err);
  [self logLine:had ? @"[rootfs] removed (rootfs-*.aotpack + manifest deleted)"
                    : @"[rootfs] nothing to remove"];
  [self refreshRootfsCard];
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
- (void)bootOption:(UIButton*)sender {
  NSArray* options = @[@"auto", @"android-7.1", @"android-8.1", @"android-10"];
  NSString* choice = options[sender.tag - 100];
  [[NSUserDefaults standardUserDefaults] setObject:choice
                                            forKey:@"AOTDroidBootOverride"];
  _profile = [[AOTDroidProfiler shared] selectProfile];
  [self logLine:[NSString stringWithFormat:@"[override] %@ -> %@ (%@ MB budget)",
      choice, _profile[@"profileTag"], _profile[@"rssBudgetMB"]]];
}

- (void)bootNow:(UIButton*)sender {
  (void)sender;
  // 1. Jetsam guard first: never allocate before the pressure hooks exist.
  aotdroid::jetsamGuardInstall(nil);

  // 2. Container boot: installed rootfs pack + binder domains + properties + init.
  aotdroid::BootConfig cfg;
  cfg.apiLevel = ((NSNumber*)_profile[@"apiLevel"]).unsignedIntValue;
  cfg.profileTag = [_profile[@"profileTag"] UTF8String];
  cfg.containerRoot = [NSHomeDirectory() UTF8String];   // app sandbox container
  aotdroid::BootResult r = aotdroid::containerBoot(cfg);
  if (r.ok) {
    [self logLine:[NSString stringWithFormat:
        @"[boot] container up (init domain %u, %zu property bytes)",
        r.initDomain, r.propertyBytes]];
    // 3. Metal presentation host at the profiled framebuffer geometry.
    aotdroid::hwcMetalAttachToView((__bridge void*)self,
                         ((NSNumber*)_profile[@"fbW"]).unsignedIntValue,
                         ((NSNumber*)_profile[@"fbH"]).unsignedIntValue,
                         96 * 1024 * 1024);
  } else {
    [self logLine:[NSString stringWithFormat:@"[boot] FAILED: %s", r.error.c_str()]];
  }
}

@end

// ============================================================================
//  Entry point: the app links from plain object files (no @main synthesis),
//  so the UIKit bootstrap is explicit.
// ============================================================================
int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass([AOTDroidAppDelegate class]));
  }
}
