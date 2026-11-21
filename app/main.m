// ChargeCap.app: battery status and the charge limit settings.
#import <UIKit/UIKit.h>
#include <errno.h>
#include <signal.h>
#include "../src/chargecap.h"

// The iOS 15 build uses the system's semantic colours, text styles and list cells, so it follows
// Dark Mode and the reader's text size. The iOS 9 build keeps the colours and fonts of its day.
#define MODERN (__IPHONE_OS_VERSION_MIN_REQUIRED >= 150000)

#if MODERN
#define COLOR_TRACK UIColor.tertiarySystemFillColor
#define COLOR_LEVEL UIColor.systemGreenColor
#define COLOR_LOW UIColor.systemRedColor
#define COLOR_TICK UIColor.labelColor
#define COLOR_SECONDARY UIColor.secondaryLabelColor
#else
static UIColor *rgb(int hex) {
    return [UIColor colorWithRed:((hex >> 16) & 255) / 255.0 green:((hex >> 8) & 255) / 255.0 blue:(hex & 255) / 255.0 alpha:1];
}
#define COLOR_TRACK rgb(0xDADADF)
#define COLOR_LEVEL rgb(0x4CD964)
#define COLOR_LOW rgb(0xFF3B30)
#define COLOR_TICK rgb(0x1C1C1E)
#define COLOR_SECONDARY rgb(0x6D6D72)
#endif

static BOOL daemonRunning(void) {
    FILE *f = fopen(CC_PID_PATH, "r");
    int pid = 0;
    if (f) {
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);
    }
    // The daemon runs as root, so from mobile a live pid answers EPERM.
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

// The battery level as a bar, with a tick where charging will pause.
@interface LevelBar : UIView
@property (nonatomic) int percent, limit;
@property (nonatomic) BOOL showLimit;
@end

@implementation LevelBar
- (void)drawRect:(CGRect)rect {
    CGRect track = self.bounds;
    UIBezierPath *shape = [UIBezierPath bezierPathWithRoundedRect:track cornerRadius:track.size.height / 2];
    [COLOR_TRACK setFill];
    [shape fill];
    [shape addClip];
    CGRect fill = track;
    fill.size.width = round(track.size.width * MAX(0, MIN(100, self.percent)) / 100.0);
    [(self.percent <= 20 ? COLOR_LOW : COLOR_LEVEL) setFill];
    UIRectFill(fill);
    if (self.showLimit) {
        CGFloat x = round(track.size.width * self.limit / 100.0);
        [COLOR_TICK setFill];
        UIRectFill(CGRectMake(x - 1, 0, 2, track.size.height));
    }
}
@end

// A full-width slider that lines up with the text in the cells around it.
@interface SliderCell : UITableViewCell
@property (strong, nonatomic) UISlider *slider;
@end

@implementation SliderCell
- (void)layoutSubviews {
    [super layoutSubviews];
    UIEdgeInsets m = self.contentView.layoutMargins;
    CGRect b = self.contentView.bounds;
    self.slider.frame = CGRectMake(m.left, 0, b.size.width - m.left - m.right, b.size.height);
}
@end

enum { SecLimit, SecBattery, SecCount };
enum { RowEnabled, RowLimit, RowSlider, RowResume, RowResumeOff, LimitRows };
enum { RowHealth, RowCycles, RowTemp, RowCurrent, BatteryRows };

@interface SettingsController : UITableViewController
- (void)reloadPrefs;
@end

@implementation SettingsController {
    cc_prefs _prefs;
    cc_battery _battery;
    int _gap; // limit minus resume, kept while the slider moves
    BOOL _daemonUp;
    UILabel *_percentLabel, *_stateLabel;
    LevelBar *_bar;
    UISwitch *_switch;
    UISlider *_slider;
    UIStepper *_stepper, *_stepperOff;
    NSArray *_limitCells, *_batteryCells;
    NSTimer *_timer;
}

- (instancetype)init {
#if MODERN
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
#else
    self = [super initWithStyle:UITableViewStyleGrouped];
#endif
    if (self) self.title = @"ChargeCap";
    return self;
}

- (UITableViewCell *)cell:(NSString *)title {
    UITableViewCell *c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
#if MODERN
    UIListContentConfiguration *config = UIListContentConfiguration.valueCellConfiguration;
    config.text = title;
    c.contentConfiguration = config;
#else
    c.textLabel.text = title;
#endif
    c.selectionStyle = UITableViewCellSelectionStyleNone;
    return c;
}

- (void)setDetail:(NSString *)detail ofCell:(UITableViewCell *)c {
#if MODERN
    UIListContentConfiguration *config = (UIListContentConfiguration *)c.contentConfiguration;
    config.secondaryText = detail;
    c.contentConfiguration = config;
#else
    c.detailTextLabel.text = detail;
    [c setNeedsLayout];
#endif
}

// Sits beside the value label, with a gap so the label stays off the stepper.
- (UIView *)stepperAccessory:(UIStepper *)stepper {
    UIView *gap = [[UIView alloc] initWithFrame:CGRectMake(0, 0, stepper.bounds.size.width + 14, stepper.bounds.size.height)];
    stepper.frame = CGRectOffset(stepper.bounds, 14, 0);
    [gap addSubview:stepper];
    return gap;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    _daemonUp = YES;

    _switch = [UISwitch new];
    [_switch addTarget:self action:@selector(toggled) forControlEvents:UIControlEventValueChanged];
    _slider = [UISlider new];
    _slider.minimumValue = CC_MIN_LIMIT;
    _slider.maximumValue = 100;
    [_slider addTarget:self action:@selector(sliding) forControlEvents:UIControlEventValueChanged];
    [_slider addTarget:self action:@selector(slid)
      forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    _stepper = [UIStepper new];
    _stepper.minimumValue = CC_MIN_RESUME;
    [_stepper addTarget:self action:@selector(stepped) forControlEvents:UIControlEventValueChanged];
    _stepperOff = [UIStepper new];
    _stepperOff.minimumValue = CC_MIN_RESUME;
    [_stepperOff addTarget:self action:@selector(steppedOff) forControlEvents:UIControlEventValueChanged];

    UITableViewCell *enabled = [self cell:@"Limit charging"];
    enabled.accessoryView = _switch;
    SliderCell *slider = [[SliderCell alloc] initWithStyle:UITableViewCellStyleDefault reuseIdentifier:nil];
    slider.selectionStyle = UITableViewCellSelectionStyleNone;
    slider.slider = _slider;
    [slider.contentView addSubview:_slider];
    UITableViewCell *resume = [self cell:@"Charge again at"];
    resume.accessoryView = [self stepperAccessory:_stepper];
    UITableViewCell *resumeOff = [self cell:@"With the screen off, at"];
    resumeOff.accessoryView = [self stepperAccessory:_stepperOff];
    _limitCells = @[ enabled, [self cell:@"Pause charging at"], slider, resume, resumeOff ];
    _batteryCells = @[ [self cell:@"Health"], [self cell:@"Charge cycles"], [self cell:@"Temperature"],
                       [self cell:@"Current"] ];

    UIView *header = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 320, 168)];
    _percentLabel = [UILabel new];
    _stateLabel = [UILabel new];
#if MODERN
    UIFont *big = [UIFont monospacedDigitSystemFontOfSize:64 weight:UIFontWeightSemibold];
    big = [UIFont fontWithDescriptor:[big.fontDescriptor fontDescriptorWithDesign:UIFontDescriptorSystemDesignRounded] size:64];
    _percentLabel.font = [[UIFontMetrics metricsForTextStyle:UIFontTextStyleLargeTitle] scaledFontForFont:big maximumPointSize:96];
    _stateLabel.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _percentLabel.adjustsFontForContentSizeCategory = YES;
    _stateLabel.adjustsFontForContentSizeCategory = YES;
    _stateLabel.numberOfLines = 0;
#else
    _percentLabel.font = [UIFont systemFontOfSize:76 weight:UIFontWeightUltraLight];
    _stateLabel.font = [UIFont systemFontOfSize:17];
#endif
    _stateLabel.textColor = COLOR_SECONDARY;
    for (UILabel *l in @[ _percentLabel, _stateLabel ]) {
        l.textAlignment = NSTextAlignmentCenter;
        [header addSubview:l];
    }
    _bar = [LevelBar new];
    _bar.backgroundColor = UIColor.clearColor;
    _bar.contentMode = UIViewContentModeRedraw;
    [header addSubview:_bar];
    self.tableView.tableHeaderView = header;

    [[NSNotificationCenter defaultCenter] addObserver:self selector:@selector(reloadPrefs)
                                                 name:UIApplicationWillEnterForegroundNotification object:nil];
    [self reloadPrefs];
}

// Stack the header to fit its text, which grows with the reader's text size, and line the
// level bar up with the content below it.
- (void)viewDidLayoutSubviews {
    [super viewDidLayoutSubviews];
    UIView *header = self.tableView.tableHeaderView;
    CGFloat width = self.tableView.bounds.size.width;
    CGFloat percentHeight = [_percentLabel sizeThatFits:CGSizeMake(width, CGFLOAT_MAX)].height;
    CGFloat stateHeight = [_stateLabel sizeThatFits:CGSizeMake(width - 40, CGFLOAT_MAX)].height;
    _percentLabel.frame = CGRectMake(0, 14, width, percentHeight);
    _stateLabel.frame = CGRectMake(20, CGRectGetMaxY(_percentLabel.frame), width - 40, stateHeight);
#if MODERN
    CGRect text = self.tableView.layoutMarginsGuide.layoutFrame;
#else
    CGRect text = self.tableView.readableContentGuide.layoutFrame;
#endif
    _bar.frame = CGRectMake(CGRectGetMinX(text), CGRectGetMaxY(_stateLabel.frame) + 14, CGRectGetWidth(text), 8);
    CGFloat height = CGRectGetMaxY(_bar.frame) + 18;
    if (header.frame.size.width != width || header.frame.size.height != height) {
        header.frame = CGRectMake(0, 0, width, height);
        self.tableView.tableHeaderView = header; // the table only rereads the height when it is set again
    }
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    _timer = [NSTimer scheduledTimerWithTimeInterval:3 target:self selector:@selector(refresh) userInfo:nil repeats:YES];
}

- (void)viewWillDisappear:(BOOL)animated {
    [super viewWillDisappear:animated];
    [_timer invalidate];
}

// Settings may have changed from the shell while we were in the background.
- (void)reloadPrefs {
    cc_prefs_load(&_prefs);
    _gap = _prefs.limit - _prefs.resume;
    _switch.on = _prefs.enabled;
    _slider.value = _prefs.limit;
    [self showPrefs];
    [self.tableView reloadData];
    [self refresh];
}

- (void)showPrefs {
    _stepper.maximumValue = _prefs.limit - 1;
    _stepper.value = _prefs.resume;
    _stepperOff.maximumValue = _prefs.resume;
    _stepperOff.value = _prefs.resume_off;
    [self setDetail:[NSString stringWithFormat:@"%d%%", _prefs.limit] ofCell:_limitCells[RowLimit]];
    [self setDetail:[NSString stringWithFormat:@"%d%%", _prefs.resume] ofCell:_limitCells[RowResume]];
    [self setDetail:[NSString stringWithFormat:@"%d%%", _prefs.resume_off] ofCell:_limitCells[RowResumeOff]];
    UITableViewHeaderFooterView *footer = [self.tableView footerViewForSection:SecLimit];
    footer.textLabel.text = [self tableView:self.tableView titleForFooterInSection:SecLimit];
    [footer setNeedsLayout];
    _bar.limit = _prefs.limit;
    _bar.showLimit = _prefs.enabled;
    [_bar setNeedsDisplay];
}

- (void)save {
    cc_prefs_clamp(&_prefs);
    if (!cc_prefs_save(&_prefs)) NSLog(@"ChargeCap: could not save %s", CC_PREFS_PATH);
    [self showPrefs];
    [self performSelector:@selector(refresh) withObject:nil afterDelay:1.5];
}

- (void)toggled {
    _prefs.enabled = _switch.on;
    NSMutableArray *rows = [NSMutableArray array];
    for (NSInteger r = RowLimit; r < LimitRows; r++) [rows addObject:[NSIndexPath indexPathForRow:r inSection:SecLimit]];
    [self.tableView beginUpdates];
    if (_prefs.enabled) [self.tableView insertRowsAtIndexPaths:rows withRowAnimation:UITableViewRowAnimationFade];
    else [self.tableView deleteRowsAtIndexPaths:rows withRowAnimation:UITableViewRowAnimationFade];
    [self.tableView endUpdates];
    [self save];
}

- (void)sliding {
    _prefs.limit = (int)lroundf(_slider.value);
    _prefs.resume = _prefs.limit - _gap;
    cc_prefs_clamp(&_prefs);
    [self showPrefs];
}

- (void)slid {
    [self sliding];
    _slider.value = _prefs.limit;
    [self save];
}

- (void)stepped {
    _prefs.resume = (int)_stepper.value;
    _gap = _prefs.limit - _prefs.resume;
    [self save];
}

- (void)steppedOff {
    _prefs.resume_off = (int)_stepperOff.value;
    [self save];
}

- (void)refresh {
    if (!cc_battery_read(&_battery)) {
        _percentLabel.text = @"–";
        _stateLabel.text = @"Can't read the battery";
        return;
    }
    _percentLabel.text = [NSString stringWithFormat:@"%d%%", _battery.percent];
    if (!_battery.external) _stateLabel.text = @"On battery";
    else if (_battery.draining) _stateLabel.text = [NSString stringWithFormat:@"Plugged in · running down to %d%%", _prefs.limit];
    else if (_battery.inhibited) _stateLabel.text = @"Plugged in · charging paused";
    else if (_battery.charging && _prefs.enabled && _battery.percent < _prefs.limit)
        _stateLabel.text = [NSString stringWithFormat:@"Charging to %d%%", _prefs.limit];
    else if (_battery.charging) _stateLabel.text = @"Charging";
    else _stateLabel.text = @"Plugged in · not charging";
    _bar.percent = _battery.percent;
    [_bar setNeedsDisplay];

    NSString *health = @"–";
    if (_battery.design_mah > 0)
        health = [NSString stringWithFormat:@"%d%% · %d of %d mAh", (_battery.max_mah * 100 + _battery.design_mah / 2) / _battery.design_mah,
                                            _battery.max_mah, _battery.design_mah];
    NSArray *values = @[ health, [NSString stringWithFormat:@"%d", _battery.cycles],
                         [NSString stringWithFormat:@"%.1f °C", _battery.temp_c100 / 100.0],
                         [NSString stringWithFormat:@"%+d mA", _battery.milliamps] ];
    for (NSUInteger i = 0; i < BatteryRows; i++) [self setDetail:values[i] ofCell:_batteryCells[i]];
    [self.view setNeedsLayout];

    BOOL up = daemonRunning();
    if (up != _daemonUp) {
        _daemonUp = up;
        [self.tableView reloadSections:[NSIndexSet indexSetWithIndex:SecBattery] withRowAnimation:UITableViewRowAnimationNone];
    }
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tv {
    return SecCount;
}

- (NSInteger)tableView:(UITableView *)tv numberOfRowsInSection:(NSInteger)section {
    if (section == SecLimit) return _prefs.enabled ? LimitRows : 1;
    return BatteryRows;
}

- (UITableViewCell *)tableView:(UITableView *)tv cellForRowAtIndexPath:(NSIndexPath *)ip {
    return ip.section == SecLimit ? _limitCells[ip.row] : _batteryCells[ip.row];
}

- (NSString *)tableView:(UITableView *)tv titleForHeaderInSection:(NSInteger)section {
    return section == SecLimit ? @"Charge limit" : @"Battery";
}

- (NSString *)tableView:(UITableView *)tv titleForFooterInSection:(NSInteger)section {
    if (section == SecLimit) {
        if (!_prefs.enabled) return @"The iPad charges to 100% as normal.";
        return [NSString stringWithFormat:@"Charging pauses at %1$d%% and starts again once the battery falls to %2$d%%, "
                                          @"or %3$d%% while the screen is off. While paused, the iPad runs from the charger."
#if MODERN
                                          @" Above %1$d%%, it runs from the battery until it is back at %1$d%%."
#endif
                                          , _prefs.limit, _prefs.resume, _prefs.resume_off];
    }
    if (!_daemonUp) return @"The ChargeCap service isn't running, so the limit isn't being applied. Reinstall the package to start it.";
    return nil;
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (strong, nonatomic) UIWindow *window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication *)app didFinishLaunchingWithOptions:(NSDictionary *)options {
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    UINavigationController *nav = [[UINavigationController alloc] initWithRootViewController:[SettingsController new]];
#if MODERN
    nav.navigationBar.prefersLargeTitles = YES;
#endif
    self.window.rootViewController = nav;
    [self.window makeKeyAndVisible];
    return YES;
}

// chargecap://snapshot saves a picture of the screen, for checking the layout over SSH.
// chargecap://snapshot/dark and /light force that appearance first.
- (BOOL)application:(UIApplication *)app openURL:(NSURL *)url options:(NSDictionary *)options {
    if (![url.host isEqualToString:@"snapshot"]) return NO;
#if MODERN
    if ([url.path isEqualToString:@"/dark"]) self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleDark;
    if ([url.path isEqualToString:@"/light"]) self.window.overrideUserInterfaceStyle = UIUserInterfaceStyleLight;
#endif
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1.5 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        UIGraphicsBeginImageContextWithOptions(self.window.bounds.size, YES, 0);
#if MODERN
        [self.window.layer renderInContext:UIGraphicsGetCurrentContext()]; // drawViewHierarchy comes out black here
#else
        [self.window drawViewHierarchyInRect:self.window.bounds afterScreenUpdates:YES];
#endif
        UIImage *image = UIGraphicsGetImageFromCurrentImageContext();
        UIGraphicsEndImageContext();
        [UIImagePNGRepresentation(image) writeToFile:@"/var/mobile/Media/ChargeCap-snapshot.png" atomically:YES];
    });
    return YES;
}
@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
    }
}
