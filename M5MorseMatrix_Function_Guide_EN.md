# M5MorseMatrix-UserDemo README

## Product Image

![](image/MorseMatrix_主图_z-9JxSJVWz.jpg)

## Feature Walkthrough

### 1. Slide the rear power switch to ON

![](image/image_NAabwKijHF.png)

### 2. After power-on, the standby screen randomly cycles between the "Diagonal" and "Loop" animations

![](image/待机背景动画图_psnNXFF3SC.jpg)

### 3. Tap the device to enter the waiting-for-input screen with dual LED breathing indicators

![](image/image_jEge8YBKCn.png)

### 3.1. Input Method 1: tap the button manually

A short tap lights a single LED. Holding the button lights two LEDs. Holding for more than eight LEDs returns to the standby screen.

![](image/手动敲击输入_CMcldiIMcO.jpg)

### 3.2. Input Method 2: send a string with the StringIR app on Cardputer-Adv

Flash the Cardputer-Adv with the `MorseMatrix-工具箱` firmware under the `FW` directory, or use M5Burner to update to Adv-0.3 or later. After rebooting with the new firmware, open the StringIR app.

![](image/image_KtMBP3Mc74.png)

Enter any visible ASCII characters in the text box.

![](image/image_EKdSybRPw0.png)

Use `FN + Up/Down` to move the selection. When the lower panel is focused, use `FN + Left/Right` to change options.

![](image/image_R0u1fB04MY.png)

![](image/image_2Jsje1ybwY.png)

Press Enter to start, then watch the IR transmission progress.

![](image/image_2k4iBwTOpo.png)

When the transmission completes, the matrix displays the string using the selected animation.

![](image/image_cCjzmFvnyQ.png)

`Normal`: continuously scrolls the string from right to left.  
`Fade`: fades characters in and out one by one.  
`Stream`: alternates single- and double-line streaming.

### 3.3. Input Method 3: send a string through a serial terminal

Limit characters to visible ASCII.

Connect USB and switch to the waiting-for-input screen (this screen automatically exits after 30 seconds).

![](image/image_tYrj4uXb9h.png)

Open a serial tool (enable `/r/n` newline) or a serial terminal (press Enter to send).

Serial tool example (enable "Send new line" `/r/n`):

![](image/image_ajUy0rVvD4.png)

Serial terminal example:

![](image/image_1KIWQS3GsX.png)

Advanced: send JSON to specify animation (also while on the waiting-for-input screen):

Examples:

```
{"text":"HELLO M5STCAK","style":"Normal"}
{"text":"HELLO M5STCAK","style":"Fade"}
{"text":"HELLO M5STCAK","style":"Stream"}
```

![](image/image_ebhDzX-ceA.png)

Tap the string display screen to return to the waiting-for-input screen. Hold the button until the dual-line breathing animation appears to go back to standby.
