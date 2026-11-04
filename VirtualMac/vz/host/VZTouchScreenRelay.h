#import <UIKit/UIKit.h>

// Returns the view showing the guest and the guest display's rectangle in it.
typedef CGRect (*VZTouchScreenViewportProvider)(UIView **view);

// Sends the fingers on the iPad's screen to the virtual machine's touch screen.
void VZTouchScreenRelayStart(id virtualMachine, VZTouchScreenViewportProvider viewportProvider);
void VZTouchScreenRelayStop(void);
BOOL VZTouchScreenRelayActive(void);
