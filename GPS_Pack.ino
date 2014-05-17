/*

 GPS Pack - reverse geocache controller
 Copyright 2013 Nicholas W. Sayer
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License along
 with this program; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
 
#include <Servo.h>
#include <TinyGPS.h>
#include <EEPROM.h>
#include <LiquidCrystal.h>
#include <avr/sleep.h>

#define GPS_BAUD 9600

#define BUTTON_PIN 2
#define BUTTON_IRQ 0
// We don't use the PPS functionality, but here's where it is
#define PPS_PIN 3
#define PPS_IRQ 1

#define POWER_EN 4
#define SERVO_CTL 5
#define SERVO_EN 6

#define LCD_E 8
#define LCD_RS 9
#define LCD_D4 10
#define LCD_D5 11
#define LCD_D6 12
#define LCD_D7 13

#define BATTERY_LEVEL A0
#define BATTERY_THRESHOLD_MV 2000 // 2 volts - at that point, the GPS will stop retaining warm-start capabilities.

#define BUTTON_DEBOUNCE_TIME 50 // milliseconds of button debounce
#define BUTTON_LONG_START 250 // 1/4 second is a "long" push
#define CHEAT_PUSH_TIME 2000 // if the wake-up push is longer than this, then enter cheat mode

#define SLEEP_INTERVAL_MS 60000 // 30 seconds with no interaction => sleep
#define FIX_TIMEOUT_MS 180000 // 3 minutes waiting for a fix => sleep

#define PMTK_SET_NMEA_UPDATE_5HZ     "$PMTK220,200*2C"
#define PMTK_SET_NMEA_OUTPUT_RMCONLY "$PMTK314,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*29"
#define PMTK_SET_NMEA_OUTPUT_RMCGGA  "$PMTK314,0,1,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0*28"
#define PMTK_SET_NMEA_OUTPUT_ALLDATA "$PMTK314,1,1,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0*28"

#define MAXIMUM_FIX_AGE 10000 // 10 seconds

// If they're more than, say, 500 kM from the target, then
// it's more likely that the fix is wrong or that the target
// was incorrectly specified. In any event, it's not a
// whole lot of fun if the target is the better part of
// a day's drive away.
#define WAY_COLD_DISTANCE 500000

#define DEGREE_CHAR ((char)(0xDF))

#define SERVO_ANGLE_LOCK 45
#define SERVO_ANGLE_UNLOCK 135
#define SERVO_TIME 1000

// button events from check_event()
#define EVENT_NONE 0
#define EVENT_SHORT_PUSH 1
#define EVENT_LONG_PUSH 2

// Mode setup has to be zero, because an "invalid mode" is aliased to 0.
// The default EEPROM content is 0xff, which is invalid.
#define MODE_SETUP 0
#define MODE_GAME 1
#define MODE_OPEN 2
#define MODE_LAST MODE_OPEN

#define EEPROM_LOC_MODE 0
#define EEPROM_LOC_CHEAT_CODE 1
#define EEPROM_LOC_TARGET_RADIUS 3
#define EEPROM_LOC_TARGET_LAT 4
#define EEPROM_LOC_TARGET_LON 8
#define EEPROM_LOC_HINT_TYPE 12

#define HINT_TYPE_BOTH 0
#define HINT_TYPE_COURSE 1
#define HINT_TYPE_DISTANCE 2
#define HINT_TYPE_NONE 3
#define HINT_TYPE_LAST HINT_TYPE_NONE

#define VERSION "v1.1"

// Thanks to Gareth Evans at http://todbot.com/blog/2008/06/19/how-to-do-big-strings-in-arduino/
// Note that you must be careful not to use this macro more than once per "statement", lest you
// risk overwriting the buffer before it is used. So no using it inside methods that return
// strings that are then used in snprintf statements that themselves use this macro.
char p_buffer[96];
#define P(str) (strcpy_P(p_buffer, PSTR(str)), p_buffer)

LiquidCrystal display(LCD_RS, LCD_E, LCD_D4, LCD_D5, LCD_D6, LCD_D7);
TinyGPS gps;

unsigned long wake_millis, lost_fix_millis;
boolean complained_signal = false;
unsigned long last_display_update = 0;
boolean enter_cheat_mode = false;
unsigned int prospective_cheat_code;
unsigned int cheat_digit;
boolean last_blink;
unsigned long button_debounce_time = 0, button_press_time = 0;
boolean initialized;
unsigned char menu_number, item_cursor, item_cursor_max, digit_number;
int target_lon_deg, target_lat_deg;
unsigned int target_lon_min, target_lat_min, target_lon_sec, target_lat_sec;
unsigned int target_lon_sec_mills, target_lat_sec_mills;
unsigned char target_lat_hemi, target_lon_hemi;

long EEPROM_read_long(int start_addr) {
  long out;
  unsigned char* memloc = (unsigned char *)&out;
  for(unsigned int i = 0; i < sizeof(out); i++)
    memloc[i] = EEPROM.read(start_addr + i);
  return out;
}
void EEPROM_write_long(int start_addr, long val) {
  unsigned char* memloc = (unsigned char *) &val;
  for(unsigned int i = 0; i < sizeof(val); i++)
    EEPROM.write(start_addr + i, memloc[i]);
}
short EEPROM_read_short(int start_addr) {
  short out;
  unsigned char* memloc = (unsigned char *)&out;
  for(unsigned int i = 0; i < sizeof(out); i++)
    memloc[i] = EEPROM.read(start_addr + i);
  return out;
}
void EEPROM_write_short(int start_addr, short val) {
  unsigned char* memloc = (unsigned char *) &val;
  for(unsigned int i = 0; i < sizeof(val); i++)
    EEPROM.write(start_addr + i, memloc[i]);
}

void set_mode(unsigned char mode) {
  EEPROM.write(EEPROM_LOC_MODE, mode);
}
unsigned char get_mode() {
  unsigned char out = EEPROM.read(EEPROM_LOC_MODE);
  if (out > MODE_LAST) out = 0;
  return out;
}

void set_lock(boolean lock) {
  // There isn't enough juice for the GPS, display AND servo.
  power_off();

  Servo servo;
  servo.attach(SERVO_CTL);
  servo.write((lock == true)?SERVO_ANGLE_LOCK:SERVO_ANGLE_UNLOCK);
  digitalWrite(SERVO_EN, HIGH);
  delay(SERVO_TIME);
  digitalWrite(SERVO_EN, LOW);
  servo.detach();
  
}

void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(BATTERY_LEVEL, INPUT);
  pinMode(SERVO_CTL, OUTPUT);
  pinMode(SERVO_EN, OUTPUT);
  pinMode(POWER_EN, OUTPUT);
  digitalWrite(SERVO_EN, LOW); // servo off
  digitalWrite(POWER_EN, HIGH); // Turn the power on
  
  // Startup banner
  display.begin(16, 2);
  display.clear();
  display.print(P("GPS Pack"));
  display.setCursor(0, 1);
  display.print(P(VERSION));
  delay(2000);
  display.clear();
  
  sleep();
}

// We don't really want the button to interrupt us, but a level interrupt is the
// most economical (power-wise) way to sleep and wake up.
void button_isr() {
  // sleep_disable() is to counter the potential race that happens if someone pushes the button
  // between enabling sleep and actually *going* to sleep. If we disable interrupts and
  // then go to sleep, it'll be The Big Sleep.
  sleep_disable();
  // We detach the interrupt because it's a level interrupt and we'll keep re-entering forever
  // if we don't.
  detachInterrupt(BUTTON_IRQ);
}

inline void power_off() {
  // If we don't write lows to all of the LCD data lines, we will attempt to
  // provide power through them. That would not be good.
  digitalWrite(LCD_E, LOW);
  digitalWrite(LCD_RS, LOW);
  digitalWrite(LCD_D4, LOW);
  digitalWrite(LCD_D5, LOW);
  digitalWrite(LCD_D6, LOW);
  digitalWrite(LCD_D7, LOW);
  // NOW turn the power off
  digitalWrite(POWER_EN, LOW);
}
void sleep() {
  power_off();
  sleep_enable();
  attachInterrupt(BUTTON_IRQ, button_isr, LOW);
  set_sleep_mode(SLEEP_MODE_PWR_DOWN);
  sleep_mode();
  
  // And here's where we wake up
  
  sleep_disable();
  detachInterrupt(BUTTON_IRQ);
  unsigned long wake_push_start = millis();
  // Wait for the button to go high with debounce
  for(long button_wait = millis(); millis() - button_wait < BUTTON_DEBOUNCE_TIME;) {
    if (digitalRead(BUTTON_PIN) == LOW) button_wait = millis();
  }

  digitalWrite(POWER_EN, HIGH);  
  display.begin(16, 2);
  display.clear();
  Serial.begin(GPS_BAUD);
  delay(100);
  Serial.println(P(PMTK_SET_NMEA_UPDATE_5HZ));
  delay(100);
  Serial.println(P(PMTK_SET_NMEA_OUTPUT_RMCGGA));
  wake_millis = millis();
  complained_signal = false;
  if (get_mode() != MODE_SETUP && millis() - wake_push_start > CHEAT_PUSH_TIME) {
    enter_cheat_mode = true;
    doCheatMenu(true);
  }
  initialized = false;
  
}

unsigned int battery_millivolts() {
  return (int)((analogRead(BATTERY_LEVEL) * 5000L) / 1024);
}

void DMSToDeg(int deg, unsigned int mins, unsigned int secs, unsigned int sec_mills, long &combined) {
  combined = deg * 100000;
  if (combined < 0) combined *= -1; // Add the fractional part without regards to sign
  combined += (((unsigned long)mins) * 100000) / 60;
  combined += (((unsigned long)secs) * 100000 + ((unsigned long)sec_mills) * 100) / (60 * 60);
  if (deg < 0) combined *= -1; // now put the sign back on it.
}

void degToDMS(long combined, int &deg, unsigned int &mins, unsigned int &secs, unsigned int &sec_mills) {
  deg = combined / 100000;
  if (combined < 0) combined *= -1;
  combined %= 100000;
  combined *= 60;
  mins = combined / 100000;
  combined %= 100000;
  combined *= 60;
  secs = combined / 100000;
  combined %= 100000;
  sec_mills = combined / 100;
}

unsigned char check_event() {
  if (button_debounce_time != 0 && millis() - button_debounce_time < BUTTON_DEBOUNCE_TIME) {
    // debounce is in progress
    return EVENT_NONE;
  } else {
    // debounce is over
    button_debounce_time = 0;
  }
  if (digitalRead(BUTTON_PIN) == LOW) {
    // Button is down
    if (button_press_time == 0) { // this is the start of a press.
      // Pushing the button resets the sleep time.
      wake_millis = button_debounce_time = button_press_time = millis();
    }
    return EVENT_NONE; // We don't know what this button-push is going to be yet
  } else {
    // Button released
    if (button_press_time == 0) return EVENT_NONE; // It wasn't down anyway.
    // We are now ending a button-push. First, start debuncing.
    button_debounce_time = millis();
    unsigned long button_pushed_time = button_debounce_time - button_press_time;
    button_press_time = 0;
    if (button_pushed_time > BUTTON_LONG_START) {
      return EVENT_LONG_PUSH;
    } else {
      return EVENT_SHORT_PUSH;
    }
  }
}

void doCheatMenu(boolean initialize) {
  unsigned int event = check_event();
  if (initialize) {
    cheat_digit = 0;
    prospective_cheat_code = 0;
    event = EVENT_LONG_PUSH; // simulate having gotten a long push to get here.
    display.clear();
    display.print(P("Enter cheat code"));
  }
  boolean blink = millis() % 1000 > 500;
  if (event == EVENT_SHORT_PUSH) {
    prospective_cheat_code += 1 << (4 * (3 - cheat_digit));
    unsigned char digit = ((prospective_cheat_code >> ( 4 * (3 - cheat_digit))) & 0xf);
    if (digit >= 0xa)
      prospective_cheat_code &= ~(0xf << ( 4 * (3 - cheat_digit))); // blank out that digit
  } else if (event == EVENT_LONG_PUSH) {
    if (cheat_digit >= 3) {
      // That's the end. Check for the correct code
      unsigned int cheat_code = EEPROM_read_short(EEPROM_LOC_CHEAT_CODE);
      if (cheat_code == prospective_cheat_code) {
        set_lock(false); // unlock
        set_mode(MODE_SETUP);
        enter_cheat_mode = false;
        sleep();
      } else {
        display.clear();
        display.setCursor(6, 0);
        display.print(P("NO!!"));
        delay(5000);
        enter_cheat_mode = false;
        sleep();
      }
      return;
    }
    if (!initialize) cheat_digit++;
  } else if (event == EVENT_NONE && last_blink == blink) {
    return;
  }
  last_blink = blink;
  // render the display
  display.setCursor(6, 1);
  for(unsigned int i = 0; i <= 3; i++) {
    char c = '0' + ((prospective_cheat_code >> ( 4 * (3 - i))) & 0xf);
    display.print((blink && i == cheat_digit)?' ':c);
  } 
}

void doGameMode() {
  char buf[17];
  float target_lon, target_lat;
  float fix_lon, fix_lat;
  gps.f_get_position(&fix_lat, &fix_lon);
  target_lat = (float)EEPROM_read_long(EEPROM_LOC_TARGET_LAT);
  target_lat /= 100000;
  target_lon = (float)EEPROM_read_long(EEPROM_LOC_TARGET_LON);
  target_lon /= 100000;
  unsigned int distance = (unsigned int)gps.distance_between(fix_lat, fix_lon, target_lat, target_lon);
  unsigned int course = (unsigned int)gps.course_to(fix_lat, fix_lon, target_lat, target_lon);
  unsigned char target_radius = EEPROM.read(EEPROM_LOC_TARGET_RADIUS);
  if (distance <= target_radius) {
    display.clear();
    display.print(P("CONGRATULATIONS!"));
    snprintf(buf, sizeof(buf), P("Look %dm %s"), distance, gps.cardinal(course));
    display.print(buf);
    delay(5000);
    set_lock(false);
    set_mode(MODE_OPEN);
    sleep();
    return;
  } else {
    if (distance > WAY_COLD_DISTANCE) {
      display.clear();
      display.print(P(" You're WAY far"));
      display.setCursor(0, 1);
      display.print(P("away from target"));
      delay(5000);
      sleep();
      return;
    }
    unsigned char hint_type = EEPROM.read(EEPROM_LOC_HINT_TYPE);
    switch(hint_type) {
      case HINT_TYPE_BOTH:
        display.clear();
        display.print(P("    Not here."));
        display.setCursor(0, 1);
        if (distance > 1000) {
          snprintf(buf, sizeof(buf), P("Go %dkm %s."), distance/1000, gps.cardinal(course));
        } else {
          snprintf(buf, sizeof(buf), P("Go %dm %s."), distance, gps.cardinal(course));
        }
        display.print(buf);
        delay(5000);
        sleep();
        break;
      case HINT_TYPE_COURSE:
        display.clear();
        display.print(P("    Not here."));
        display.setCursor(0, 1);
        snprintf(buf, sizeof(buf), P("It's %s of here."), gps.cardinal(course));
        display.print(buf);
        delay(5000);
        sleep();
        break;
      case HINT_TYPE_DISTANCE:
        display.clear();
        display.print(P("    Not here."));
        display.setCursor(0, 1);
        if (distance > 1000) {
          snprintf(buf, sizeof(buf), P("It's %dkm away."), distance/1000);
        } else {
          snprintf(buf, sizeof(buf), P("It's %dm away."), distance);
        }
        display.print(buf);
        delay(5000);
        sleep();
        break;
      default:
        display.clear();
        display.print(P("    Not here."));
        display.setCursor(0, 1);
        display.print(P(" Try elsewhere."));
        delay(5000);
        sleep();
        return;
    }
  }
}

void doSetupMode() {
  unsigned char event = check_event();
  boolean blink = millis() % 1000 > 500;
  if (!initialized) {
    initialized = true;
    menu_number = 99;
    event = EVENT_LONG_PUSH; // Simulate a long push
  }
  if (event == EVENT_SHORT_PUSH) {
    item_cursor++;
    if (item_cursor > item_cursor_max) item_cursor = 0;
    switch(menu_number) {
      // For the editors, we need to "put away" the selected digit
      case 0:
        // here, we need to just simulate a long push without the commit action.
        digit_number = 99;
        goto SKIP_TO_EDITOR_HACK;
      case 1:
        switch(digit_number) {
            case 0:
              target_lat_deg = target_lat_deg % 10 + (item_cursor * 10);
              break;
            case 1:
              target_lat_deg = (target_lat_deg / 10) * 10 + item_cursor;
              break;
            case 2:
              target_lat_min = target_lat_min % 10 + (item_cursor * 10);
              break;
            case 3:
              target_lat_min = (target_lat_min / 10) * 10 + item_cursor;
              break;
            case 4:
              target_lat_sec = target_lat_sec % 10 + (item_cursor * 10);
              break;
            case 5:
              target_lat_sec = (target_lat_sec / 10) * 10 + item_cursor;
              break;
            case 6:
              target_lat_sec_mills = item_cursor * 100;
              break;
            case 7:
              target_lat_hemi = item_cursor;
              break;
            case 8:
              target_lon_deg -= (target_lon_deg / 100) * 100;
              target_lon_deg += item_cursor * 100;
              break;
            case 9:
              target_lon_deg -= ((target_lon_deg / 10) % 10) * 10;
              target_lon_deg += item_cursor * 10;
              break;
            case 10:
              target_lon_deg = (target_lon_deg / 10) * 10 + item_cursor;
              break;
            case 11:
              target_lon_min = target_lon_min % 10 + (item_cursor * 10);
              break;
            case 12:
              target_lon_min = (target_lon_min / 10) * 10 + item_cursor;
              break;
            case 13:
              target_lon_sec = target_lon_sec % 10 + (item_cursor * 10);
              break;
            case 14:
              target_lon_sec = (target_lon_sec / 10) * 10 + item_cursor;
              break;
            case 15:
              target_lon_sec_mills = item_cursor * 100;
              break;
            case 16:
              target_lon_hemi = item_cursor;
              break;
        }
        break;
      case 4:
        prospective_cheat_code &= ~(0xf << ((3 - digit_number) * 4));
        prospective_cheat_code |= item_cursor << ((3 - digit_number) * 4);
        break;
    }
  } else if (event == EVENT_LONG_PUSH) {
    // First, commit whatever we just finished.
    switch(menu_number) {
      case 0: // show fix
        // a long push means commit the current fix as the target before
        // going into the editor.
        display.clear();
        display.print(P(" Setting target"));
        display.setCursor(4, 1);
        display.print(P("to HERE."));
        delay(2000);
        {
          long target_lat, target_lon;
          gps.get_position(&target_lat, &target_lon);
          EEPROM_write_long(EEPROM_LOC_TARGET_LAT, target_lat);
          EEPROM_write_long(EEPROM_LOC_TARGET_LON, target_lon);
        }
        digit_number = 99; // set up the target editor
        break;
      case 1: // target editor
        if (digit_number >= 16) { // We're done
          if (target_lat_hemi == 1) target_lat_deg *= -1;
          if (target_lon_hemi == 1) target_lon_deg *= -1;
          {
            long target_lat, target_lon;
            DMSToDeg(target_lat_deg, target_lat_min, target_lat_sec, target_lat_sec_mills, target_lat);
            DMSToDeg(target_lon_deg, target_lon_min, target_lon_sec, target_lon_sec_mills, target_lon);
            EEPROM_write_long(EEPROM_LOC_TARGET_LAT, target_lat);
            EEPROM_write_long(EEPROM_LOC_TARGET_LON, target_lon);
          }
        } else {
          menu_number--; // fake out the increment to come.
        }
        break;
      case 2: // radius
        EEPROM.write(EEPROM_LOC_TARGET_RADIUS, (item_cursor + 1) * 10);
        break;
      case 3: // hint type
        EEPROM.write(EEPROM_LOC_HINT_TYPE, item_cursor);
        digit_number = 99; // set up for the cheat code
        break;
      case 4: // cheat code
        if (digit_number >= 3) {
          EEPROM_write_short(EEPROM_LOC_CHEAT_CODE, prospective_cheat_code);
        } else {
          menu_number--; // faxe out the increment to come.
        }
        break;
      case 5: // exit & lock?
        if (item_cursor == 1) {
          set_mode(MODE_OPEN);
          sleep();
          return;
        } else {
          // do nothing. We will wrap around to the start again.
        }
        break;
    }
SKIP_TO_EDITOR_HACK:
    // Now move to the next menu, rolling around if necessary
    menu_number++;
    if (menu_number > 5) {
      menu_number = 0;
    }
    // Now initialize the new menu to play with.
    switch(menu_number) {
      case 0: // show fix
        break;
      case 1: // target editor
        item_cursor = 0;
        item_cursor_max = 9; // On the first page, we're editing digits
        if (digit_number > 15) {
          digit_number = 0;
          long target_lat, target_lon;
          target_lat = EEPROM_read_long(EEPROM_LOC_TARGET_LAT);
          target_lon = EEPROM_read_long(EEPROM_LOC_TARGET_LON);
          degToDMS(target_lat, target_lat_deg, target_lat_min, target_lat_sec, target_lat_sec_mills);
          degToDMS(target_lon, target_lon_deg, target_lon_min, target_lon_sec, target_lon_sec_mills);
          if (target_lat_deg < 0) {
            target_lat_hemi = 1;
            target_lat_deg *= -1;
          } else {
            target_lat_hemi = 0;
          }          
          if (target_lon_deg < 0) {
            target_lon_hemi = 1;
            target_lon_deg *= -1;
          } else {
            target_lon_hemi = 0;
          }
        } else {
          digit_number++;
        }
        // Now, figure out and set item_cursor and item_cursor_max for the next digit
        switch(digit_number) {
            case 0:
              item_cursor = target_lat_deg / 10;
              item_cursor_max = 9;
              break;
            case 1:
              item_cursor = target_lat_deg % 10;
              break;
            case 2:
              item_cursor_max = 5;              
              item_cursor = target_lat_min / 10;
              break;
            case 3:
              item_cursor_max = 9;              
              item_cursor = target_lat_min % 10;
              break;
            case 4:
              item_cursor_max = 5;              
              item_cursor = target_lat_sec / 10;
              break;
            case 5:
              item_cursor_max = 9;              
              item_cursor = target_lat_sec % 10;
              break;
            case 6:
              item_cursor = target_lat_sec_mills / 100;
              break;
            case 7:
              item_cursor_max = 1;
              item_cursor = target_lat_hemi;
              break;
            case 8:
              item_cursor_max = 1;
              item_cursor = target_lon_deg / 100;
              break;
            case 9:
              item_cursor_max = 9;              
              item_cursor = (target_lon_deg / 10) % 10;
              break;
            case 10:
              item_cursor = target_lon_deg % 10;
              break;
            case 11:
              item_cursor_max = 5;
              item_cursor = target_lon_min / 10;
              break;
            case 12:
              item_cursor_max = 9;              
              item_cursor = target_lon_min % 10;
              break;
            case 13:
              item_cursor_max = 5;
              item_cursor = target_lon_sec / 10;
              break;
            case 14:
              item_cursor_max = 9;
              item_cursor = target_lon_sec % 10;
              break;
            case 15:
              item_cursor = target_lon_sec_mills / 100;
              break;
            case 16:
              item_cursor_max = 1;
              item_cursor = target_lon_hemi;
              break;
        }
        break;
      case 2:
        item_cursor = 0;
        item_cursor_max = 9;
        break;
      case 3:
        item_cursor = EEPROM.read(EEPROM_LOC_HINT_TYPE);
        item_cursor_max = HINT_TYPE_LAST;
        if (item_cursor > item_cursor_max) item_cursor = 0;
        break;
      case 4:
        if (digit_number >= 4) {
          //initialize
          prospective_cheat_code == EEPROM_read_short(EEPROM_LOC_CHEAT_CODE);
          // Make sure it's a valid BCD number
          if (((prospective_cheat_code >> 0) & 0xf) >= 0xa) prospective_cheat_code &= 0xfff0;
          if (((prospective_cheat_code >> 4) & 0xf) >= 0xa) prospective_cheat_code &= 0xff0f;
          if (((prospective_cheat_code >> 8) & 0xf) >= 0xa) prospective_cheat_code &= 0xf0ff;
          if (((prospective_cheat_code >> 12) & 0xf) >= 0xa) prospective_cheat_code &= 0x0fff;
          digit_number = 0;
          item_cursor_max = 9;
        } else {
          digit_number++;
        }
        item_cursor = (prospective_cheat_code >> ( 4 * (3 - digit_number))) & 0xf;
        break;
      case 5:
        // This is a yes/no
        item_cursor = 0;
        item_cursor_max = 1;
        break;
    }
  } else if (event == EVENT_NONE && blink == last_blink) {
    return;
  }
  last_blink = blink;
  // now render the display
  display.clear();
  switch(menu_number) {
    case 0:
      long lat, lon;
      gps.get_position(&lat, &lon);
      int deg;
      unsigned int minutes;
      unsigned int seconds;
      unsigned int sec_mills;
      degToDMS(lat, deg, minutes, seconds, sec_mills);
      char buf[17];
      snprintf(buf, sizeof(buf), P("%3d%c %2d' %2d.%1d\" %c"), abs(deg), DEGREE_CHAR, minutes, seconds, sec_mills/100, (deg<0)?'S':'N');
      display.print(buf);
      display.setCursor(0, 1);
      degToDMS(lon, deg, minutes, seconds, sec_mills);
      snprintf(buf, sizeof(buf), P("%3d%c %2d' %2d.%1d\" %c"), abs(deg), DEGREE_CHAR, minutes, seconds, sec_mills/100, (deg<0)?'W':'E');
      display.print(buf);
      break;
    case 1:
      if (digit_number <= 7) {
        display.print(P("Target Lat."));
        display.setCursor(0, 1);
        display.print(' ');
        if (digit_number == 0 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_deg / 10));
        if (digit_number == 1 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_deg % 10));
        display.print(DEGREE_CHAR);
        display.print(' ');
        if (digit_number == 2 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_min / 10));
        if (digit_number == 3 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_min % 10));
        display.print('\'');
        display.print(' ');
        if (digit_number == 4 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_sec / 10));
        if (digit_number == 5 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_sec % 10));
        display.print('.');
        if (digit_number == 6 && blink)
          display.print(' ');
        else
          display.print((int)(target_lat_sec_mills / 100));
        display.print('"');
        display.print(' ');
        if (digit_number == 7 && blink)
          display.print(' ');
        else
          display.print(target_lat_hemi == 0?'N':'S');
      } else {
        display.print(P("Target Long."));
        display.setCursor(0, 1);
        if (digit_number == 8 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_deg / 100));
        if (digit_number == 9 && blink)
          display.print(' ');
        else
          display.print((int)((target_lon_deg / 10) % 10));
        if (digit_number == 10 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_deg % 10));
        display.print(DEGREE_CHAR);
        display.print(' ');
        if (digit_number == 11 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_min / 10));
        if (digit_number == 12 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_min % 10));
        display.print('\'');
        display.print(' ');
        if (digit_number == 13 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_sec / 10));
        if (digit_number == 14 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_sec % 10));
        display.print('.');
        if (digit_number == 15 && blink)
          display.print(' ');
        else
          display.print((int)(target_lon_sec_mills / 100));
        display.print('"');
        display.print(' ');
        if (digit_number == 16 && blink)
          display.print(' ');
        else
          display.print(target_lon_hemi == 0?'E':'W');
      }
      break;
    case 2:
      display.print(P("Select targ rad."));
      display.setCursor(0, 1);
      if (item_cursor != 9) display.print(' ');
      display.print((item_cursor + 1) * 10);
      display.print(P(" meters"));
      break;
    case 3:
      display.print(P("Select hint type"));
      display.setCursor(0, 1);
      switch(item_cursor) {
        case HINT_TYPE_BOTH:
          display.print(P("both dist & dir"));
          break;
        case HINT_TYPE_COURSE:
          display.print(P("Direction only"));
          break;
        case HINT_TYPE_DISTANCE:
          display.print(P("Distance only"));
          break;
        case HINT_TYPE_NONE:
          display.print(P("No hints"));
          break;
      }
      break;
    case 4:
      display.print(P("Enter cheat code"));
      display.setCursor(6, 1);
      for(unsigned int i = 0; i <= 3; i++) {
        char c = '0' + ((prospective_cheat_code >> ( 4 * (3 - i))) & 0xf);
        display.print((blink && i == digit_number)?' ':c);
      } 
      break;
    case 5:
      display.print(P("Exit setup mode?"));
      display.setCursor(0, 1);
      switch(item_cursor) {
        case 0:
          display.print(P("No"));
          break;
        case 1:
          display.print(P("Yes"));
          break;
      }
      break;
  }
}

void doOpenMode() {
  if (!initialized) {
    initialized = true;
    display.clear();
    display.print(P(" Ready to LOCK?"));
    display.setCursor(0, 1);
    display.print(P("Long push = YES"));
  }
  unsigned int event = check_event();
  switch(event) {
    case EVENT_SHORT_PUSH:
      sleep();
      return;
    case EVENT_LONG_PUSH:
      set_lock(true);
      set_mode(MODE_GAME);
      sleep();
      return;
  }
}

void loop() {
  
  if (battery_millivolts() < BATTERY_THRESHOLD_MV) {
    display.clear();
    display.print(P("Low battery"));
    delay(2000);
    sleep();
    return;
  }

  if (millis() - wake_millis > SLEEP_INTERVAL_MS) {
    sleep();
    return;
  }
  
  // Feed the beast
  while(Serial.available()) {
    unsigned char c = Serial.read();
    //display.print((char)c);
    gps.encode(c);
  }

  if (enter_cheat_mode) {
    doCheatMenu(false);
    return;
  }
  
  // Now, do we have a fix yet?
  long lat, lon;
  unsigned long age;
  gps.get_position(&lat, &lon, &age);
  if (age > MAXIMUM_FIX_AGE) {
    // don't go to sleep while we're trying to get a fix
    wake_millis = millis();
    if (!complained_signal) {
      complained_signal = true;
      lost_fix_millis = millis();
      display.clear();
      display.print(P("Waiting for fix..."));
    } else {
      if (millis() - lost_fix_millis > FIX_TIMEOUT_MS) {
        display.clear();
        display.print(P("Fix timeout."));
        display.setCursor(0, 1);
        display.print(P("Try again..."));
        delay(5000);
        sleep();
        return;
      }
    }
    return;
  } else {
    complained_signal = false;
  }
  unsigned char mode = get_mode();
  switch(mode) {
    case MODE_GAME:
      doGameMode();
      return;
      break;
    case MODE_SETUP:
      doSetupMode();
      return;
      break;
    case MODE_OPEN:
      doOpenMode();
      return;
      break;
  }
}
