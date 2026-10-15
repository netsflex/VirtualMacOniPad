#import "VZRestoreCatalog.h"

static NSString * const VZCatalogURL =
    @"https://api.virtualbuddy.app/v2/restore/mac?apiKey=15A25D48-4A34-4EE4-A293-C22B0DE1B54E";

static NSString *VZRestoreString(id value)
{
    return [value isKindOfClass:NSString.class] ? value : @"";
}

// Keep every release identity in one table. Catalog grouping, user-facing
// names, restore-image inference, installer artwork, and library wallpaper
// selection must all resolve through these records so a newly supported
// macOS release cannot acquire conflicting names in different screens.
static NSArray<NSDictionary *> *VZMacOSReleaseIdentities(void)
{
    static NSArray *identities;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        identities = [@[
            @{ @"major": @12, @"group": @"monterey",
               @"marketing": @"Monterey", @"artwork": @"monterey",
               @"aliases": @[@"monterey"] },
            @{ @"major": @13, @"group": @"ventura",
               @"marketing": @"Ventura", @"artwork": @"ventura",
               @"aliases": @[@"ventura"] },
            @{ @"major": @14, @"group": @"sonoma",
               @"marketing": @"Sonoma", @"artwork": @"sonoma",
               @"aliases": @[@"sonoma"] },
            @{ @"major": @15, @"group": @"sequoia",
               @"marketing": @"Sequoia", @"artwork": @"sequoia",
               @"aliases": @[@"sequoia"] },
            @{ @"major": @26, @"group": @"tahoe",
               @"marketing": @"Tahoe", @"artwork": @"tahoe",
               @"aliases": @[@"tahoe"] },
            @{ @"major": @27, @"group": @"goldengate",
               @"marketing": @"Golden Gate", @"artwork": @"golden-gate",
               @"aliases": @[@"golden gate", @"golden-gate",
                                @"goldengate"] },
        ] retain];
    });
    return identities;
}

static NSDictionary *VZMacOSIdentityForMajor(NSInteger major)
{
    for (NSDictionary *identity in VZMacOSReleaseIdentities())
        if ([identity[@"major"] integerValue] == major)
            return identity;
    return nil;
}

static NSDictionary *VZMacOSIdentityForGroup(NSString *group)
{
    NSString *lower = group.lowercaseString;
    for (NSDictionary *identity in VZMacOSReleaseIdentities()) {
        if ([identity[@"group"] isEqualToString:lower])
            return identity;
        for (NSString *alias in identity[@"aliases"])
            if ([alias isEqualToString:lower])
                return identity;
    }
    return nil;
}

static NSDictionary *VZMacOSIdentityInText(NSString *text)
{
    NSString *lower = text.lowercaseString;
    for (NSDictionary *identity in VZMacOSReleaseIdentities())
        for (NSString *alias in identity[@"aliases"])
            if ([lower containsString:alias])
                return identity;
    return nil;
}

static NSString *VZRestoreVersionFromText(NSString *text)
{
    NSRegularExpression *expression = [NSRegularExpression
        regularExpressionWithPattern:@"[0-9]+(?:\\.[0-9]+){0,2}"
        options:0 error:nil];
    NSTextCheckingResult *match = [expression firstMatchInString:text
        options:0 range:NSMakeRange(0, text.length)];
    return match ? [text substringWithRange:match.range] : @"";
}

static NSInteger VZRestoreMajorVersionFromText(NSString *text)
{
    NSDictionary *identity = VZMacOSIdentityInText(text);
    if (identity)
        return [identity[@"major"] integerValue];
    NSString *version = VZRestoreVersionFromText(text);
    NSScanner *scanner = [NSScanner scannerWithString:version];
    NSInteger major = 0;
    if (![scanner scanInteger:&major] || major < 10 || major > 99)
        return 0;
    return major;
}

static NSDictionary *VZMacOSIdentityForImage(NSDictionary *image)
{
    NSDictionary *identity = VZMacOSIdentityForGroup(
        VZRestoreString(image[@"group"]));
    if (identity)
        return identity;
    NSInteger major = VZRestoreMajorVersionFromText(
        VZRestoreString(image[@"version"]));
    if (major == 0)
        major = VZRestoreMajorVersionFromText(VZRestoreString(image[@"name"]));
    return VZMacOSIdentityForMajor(major);
}

@implementation VZRestoreCatalog

+ (NSArray *)imagesFromData:(NSData *)data error:(NSError **)error
{
    if (!data.length) return nil;
    NSDictionary *root = [NSJSONSerialization JSONObjectWithData:data options:0 error:error];
    NSArray *images = [root isKindOfClass:NSDictionary.class]
        ? root[@"restoreImages"] : nil;
    if (![images isKindOfClass:NSArray.class])
        return nil;
    NSMutableArray *normalized = [NSMutableArray array];
    for (id candidate in images) {
        if (![candidate isKindOfClass:NSDictionary.class])
            continue;
        NSMutableDictionary *image = [NSMutableDictionary
            dictionaryWithDictionary:candidate];
        NSString *urlString = VZRestoreString(image[@"url"]);
        NSURL *url = [NSURL URLWithString:urlString];
        if (!url.host.length ||
            !([url.scheme.lowercaseString isEqualToString:@"https"] ||
              [url.scheme.lowercaseString isEqualToString:@"http"]))
            continue;
        NSString *name = VZRestoreString(image[@"name"]);
        NSString *version = VZRestoreString(image[@"version"]);
        if (!version.length)
            version = VZRestoreVersionFromText(name);
        NSString *group = VZRestoreString(image[@"group"]).lowercaseString;
        NSDictionary *groupIdentity = VZMacOSIdentityForGroup(group);
        if (groupIdentity) {
            group = groupIdentity[@"group"];
        } else if (!group.length) {
            NSInteger major = VZRestoreMajorVersionFromText(version);
            NSDictionary *identity = VZMacOSIdentityForMajor(major);
            group = identity[@"group"] ?: (major > 0
                ? [NSString stringWithFormat:@"macos-%ld", (long)major]
                : @"unknown");
        }
        NSString *channel = VZRestoreString(image[@"channel"]).lowercaseString;
        if (![channel isEqualToString:@"regular"] &&
            ![channel isEqualToString:@"devbeta"])
            channel = [channel containsString:@"beta"] ? @"devbeta" : @"regular";
        if (!name.length)
            name = version.length ? [NSString stringWithFormat:@"macOS %@", version]
                                  : url.lastPathComponent;
        image[@"url"] = url.absoluteString;
        image[@"name"] = name ?: @"macOS";
        image[@"version"] = version ?: @"";
        image[@"group"] = group;
        image[@"channel"] = channel;
        image[@"build"] = VZRestoreString(image[@"build"]);
        image[@"downloadSize"] = @([image[@"downloadSize"] longLongValue]);
        [normalized addObject:image];
    }
    return normalized;
}

+ (void)loadWithCompletion:(void (^)(NSArray<NSDictionary *> *, NSError *))completion
{
    NSURL *url = [NSURL URLWithString:VZCatalogURL];
    [[[NSURLSession sharedSession] dataTaskWithURL:url
        completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
        NSError *parseError = nil;
        NSArray *images = [self imagesFromData:data error:&parseError];
        if (!images.count) {
            NSString *fallback = [NSBundle.mainBundle pathForResource:@"ipsws_v2" ofType:@"json"];
            NSData *fallbackData = [NSData dataWithContentsOfFile:fallback];
            images = [self imagesFromData:fallbackData error:&parseError];
        }
        NSArray *result = images ?: @[];
        dispatch_async(dispatch_get_main_queue(), ^{
            completion(result, result.count ? nil : (error ?: parseError));
        });
    }] resume];
}

+ (NSDictionary *)recommendedImageFromImages:(NSArray<NSDictionary *> *)images
{
    NSArray *matches = [images filteredArrayUsingPredicate:[NSPredicate predicateWithBlock:
        ^BOOL(NSDictionary *image, NSDictionary *bindings) {
            (void)bindings;
            return [VZRestoreString(image[@"group"]) isEqualToString:@"sequoia"] &&
                [VZRestoreString(image[@"channel"]) isEqualToString:@"regular"];
        }]];
    if (!matches.count)
        matches = [images filteredArrayUsingPredicate:[NSPredicate
            predicateWithBlock:^BOOL(NSDictionary *image,
                                      NSDictionary *bindings) {
            (void)bindings;
            return [VZRestoreString(image[@"channel"])
                isEqualToString:@"regular"];
        }]];
    return [matches sortedArrayUsingComparator:^NSComparisonResult(NSDictionary *left, NSDictionary *right) {
        return [VZRestoreString(right[@"version"])
            compare:VZRestoreString(left[@"version"]) options:NSNumericSearch];
    }].firstObject;
}

+ (NSArray<NSString *> *)orderedGroupsForImages:(NSArray<NSDictionary *> *)images
{
    NSMutableArray *preferred = [NSMutableArray array];
    for (NSDictionary *identity in
            [VZMacOSReleaseIdentities() reverseObjectEnumerator])
        [preferred addObject:identity[@"group"]];
    NSMutableArray *result = [NSMutableArray array];
    NSMutableSet *present = [NSMutableSet set];
    for (NSDictionary *image in images) {
        NSString *group = VZRestoreString(image[@"group"]);
        if (group.length)
            [present addObject:group];
    }
    for (NSString *group in preferred) if ([present containsObject:group]) [result addObject:group];
    NSArray *unknown = [[present allObjects]
        sortedArrayUsingSelector:@selector(localizedCaseInsensitiveCompare:)];
    for (NSString *group in unknown)
        if (![result containsObject:group]) [result addObject:group];
    return result;
}

+ (BOOL)isExperimentalImage:(NSDictionary *)image
{
    // macOS 27 runs as well as macOS 26 with the macOS 27 graphics backend.
    // Only versions this build does not support still carry the warning.
    return [self isUnsupportedImage:image];
}

+ (BOOL)isUnsupportedImage:(NSDictionary *)image
{
    return [self majorVersionForImage:image] >= 28;
}

+ (BOOL)enablesOpenGLByDefaultForImage:(NSDictionary *)image
{
    NSInteger major = [self majorVersionForImage:image];
    return major == 0 || (major >= 14 && major < 28);
}

+ (NSInteger)majorVersionForImage:(NSDictionary *)image
{
    NSDictionary *identity = VZMacOSIdentityForImage(image);
    if (identity)
        return [identity[@"major"] integerValue];
    NSString *version = VZRestoreString(image[@"version"]);
    if (!version.length)
        version = VZRestoreString(image[@"name"]);
    return VZRestoreMajorVersionFromText(version);
}

+ (NSString *)versionForImage:(NSDictionary *)image
{
    NSString *version = VZRestoreString(image[@"version"]);
    return version.length ? version
        : VZRestoreVersionFromText(VZRestoreString(image[@"name"]));
}

+ (NSString *)marketingNameForImage:(NSDictionary *)image
{
    return VZMacOSIdentityForImage(image)[@"marketing"];
}

+ (NSString *)displayNameForImage:(NSDictionary *)image compact:(BOOL)compact
{
    NSString *marketing = [self marketingNameForImage:image];
    NSInteger major = [self majorVersionForImage:image];
    if (compact) {
        if (marketing.length && major >= 27)
            return [NSString stringWithFormat:@"macOS %ld %@",
                (long)major, marketing];
        if (marketing.length && major > 0)
            return [NSString stringWithFormat:@"macOS %@ %ld",
                marketing, (long)major];
        if (major > 0)
            return [NSString stringWithFormat:@"macOS %ld", (long)major];
    }
    NSString *version = [self versionForImage:image];
    if ([version hasSuffix:@".0"])
        version = [version substringToIndex:version.length - 2];
    if (marketing.length)
        return version.length
            ? [NSString stringWithFormat:@"macOS %@ %@", marketing, version]
            : [NSString stringWithFormat:@"macOS %@", marketing];
    NSString *name = VZRestoreString(image[@"name"]);
    if (name.length)
        return name;
    return version.length ? [NSString stringWithFormat:@"macOS %@", version]
                          : @"macOS";
}

+ (NSString *)macOSNameForImage:(NSDictionary *)image
{
    NSString *marketing = [self marketingNameForImage:image];
    NSInteger major = [self majorVersionForImage:image];
    if (marketing.length)
        return [@"macOS " stringByAppendingString:marketing];
    return major > 0 ? [NSString stringWithFormat:@"macOS %ld", (long)major]
                     : @"macOS";
}

+ (NSString *)artworkNameForImage:(NSDictionary *)image
{
    return VZMacOSIdentityForImage(image)[@"artwork"] ?: @"ipsw";
}

+ (NSString *)artworkNameForMachineName:(NSString *)name
{
    NSDictionary *identity = VZMacOSIdentityInText(name);
    if (!identity)
        identity = VZMacOSIdentityForMajor(
            VZRestoreMajorVersionFromText(name));
    return identity[@"artwork"] ?: @"tiger";
}

+ (NSString *)defaultVirtualMachineNameForRestoreImagePath:(NSString *)path
{
    NSString *base = path.lastPathComponent.stringByDeletingPathExtension;
    NSDictionary *identity = VZMacOSIdentityInText(base);
    if (identity)
        return identity[@"marketing"];
    NSInteger major = VZRestoreMajorVersionFromText(base);
    identity = VZMacOSIdentityForMajor(major);
    if (identity)
        return identity[@"marketing"];
    if (major > 0)
        return [NSString stringWithFormat:@"macOS %ld", (long)major];
    return base.length ? base : @"macOS";
}

@end
