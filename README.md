GPS_Pack
========

This is the firmware for the GPS Pack reverse geocache controller from Geppetto Electronics.

The GPS Pack is a "backpack" board - it's 80x36 mm and is intended to mount on the back of an 80x36 mm 2x16 LCD character display. The board is powered by two alkaline primary cells (whether you use AAA, AA C or D only influences how long the batteries will last). There is an on-board boost converter to make 5 volts and a 3.3 volt LDO regulator to supply 3.3 volts to the AdaFruit Ultimate GPS module that's on the board. Battery power is supplied to the GPS backup pin directly from the battery, and 5 volts from the boost converter is supplied continuously to the controller, but power to the main power of the GPS module and the display is switched under software control. There is a single button for the user interface and a servo output intended to be used to lock the cache box.

When the controller is sleeping, battery draw is less than 1 mA. When the GPS and display are operating, battery draw is around 150 mA. Under these conditions, a pair of 2000 mA-hr AA batteries will last around 3 months of stand-by and around 12 hours of operation. The controller also has an analog pin sampling the battery voltage and will stop playing the game when the voltage drops to 2 volts. There is an "emergency" way in in case the batteries die while the box is locked, so it should be safe to require the box to be opened to change the batteries. The game parameters are saved in EEPROM, so conversely, you can change the batteries without requiring any reprogramming.

In general, the button operates with either "short" or "long" pushes. The boundary between the two is a quarter second (250 ms).

There are three basic modes. When the device is initially powered up, it is in "setup" mode. In this mode, the first thing displayed after a fix is obtained is the current position. You may program the current fix as the target if you wish with a "long" push of the button. If instead you use a "short" push, you will enter the target programming screen.

In the target programming screen, the first thing displayed is the target latitude in degrees, minutes, and seconds. You edit the latitude one digit at a time, with short pushes selecting the digits and a long push moving to the next digit to edit. Once you're done with the latitude, you will be presented the longitude. If you make a mistake, just press on - the menu system is circular, so you'll come back around and can fix it.

Once the target is entered, the next screen asks for a target radius. This governs how close the user must get to the target for them to "win" and the box to open. Your choices here are 10-100 meters in 10 meter steps. Be careful requiring precise fixes. GPS at its best has only around a 5 meter accuracy. If you demand a 10 meter radius, it may jump around, making the victory unpredictable.

The next screen asks for a "hint type." When the game is on and you wake the box at the wrong location, this screen chooses what sort of feedback the holder will get. You can elect to give them either the direction, as a cardinal compass point (N, ESE, SW, etc), a distance in meters (or km), both or neither (in other words, they must figure out some other way how to find the correct spot).

Next, you'll be asked to enter a 4 digit "cheat code." This is how you get back into the setup mode once you've exitted it. More on that later.

Lastly, you'll be asked if you are ready to exit the menus. If you say "yes," then the system will turn off and the next time you wake it up, it will be in "open" mode. If you say "no" to the menu, then it will start over again.

In "open" mode, the box is unlocked. Waking it up will ask if you are ready to close the box or not. A long push will lock the box and begin the game.

In "game" mode, when the box is woken, it will acquire a fix and then give feedback. If they're at the right spot, the screen will display "CONGRATULATIONS" and then the servo will open, returning the unit to "open" mode. If they're in the wrong spot, the appropriate level of hint (or not) will be given and the unit will go back to sleep.

In either "open" mode or "game" mode, you can get back to setup mode by entering the "cheat code." To do so, while the unit is asleep, hold the button down for at least 5 seconds. When it wakes up, it will ask for the "cheat code." If it matches what was set in the setup mode, the box will unlock and the next time it wakes up, it will be back in setup mode.

The recommended servo is the AdaFruit Micro Servo, aka Tower Pro SG92R. It's important to make sure that the locking mechanism places as little strain on the servo as possible. The batteries and boost converter are *just* beefy enough for it to work, but not if it needs to overcome significant mechanical binding.

If you want to hack on the firmware yourself take note of a couple of power related restrictions:

1. If you turn off the power to the display and GPS unit, be sure you write LOW to the LCD pins first. There are pullup resistors in the LCD that will wind up supplying power derived from the controller digital output pins. Not good.
2. Don't try to operate the servo with the GPS and LCD power on. There isn't enough juice.
3. Fuse the controller for minimum brownout. When the servo kicks in, the voltage will drop precipitously.

The sketch will need the TinyGPS library to be installed. All the other libraries ship with the Arduino IDE.

