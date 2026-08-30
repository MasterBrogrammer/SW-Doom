#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *want = argc > 1 ? argv[1] : "SW-Doom";
    CFArrayRef list;
    CFIndex n;
    CFIndex i;

    list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list)
	return 1;
    n = CFArrayGetCount(list);
    for (i = 0; i < n; i++)
    {
	CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
	CFStringRef name = CFDictionaryGetValue(d, kCGWindowName);
	CFNumberRef num;
	char buf[256];
	int id;

	if (!name)
	    continue;
	if (!CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8))
	    continue;
	if (!strstr(buf, want))
	    continue;
	num = CFDictionaryGetValue(d, kCGWindowNumber);
	id = 0;
	if (num)
	    CFNumberGetValue(num, kCFNumberIntType, &id);
	printf("%d\n", id);
	CFRelease(list);
	return 0;
    }
    CFRelease(list);
    return 1;
}
