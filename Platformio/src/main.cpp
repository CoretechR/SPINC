// SPINC AA Charger Firmware
// 2024 Maximilian Kern

#include <Arduino.h>
#include <Adafruit_VCNL4040.h>
#include <Servo.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SharpMem.h>
#include <lvgl.h>
#include <hardware/rtc.h>

//#define DEBUGDISPLAY

// Pin assignment -----------------------------------------------------------------------------------------------------------------------

#define SW_A 2 // SW_2
#define SW_B 0 // SW_1
#define USER_LED 14 // 25 for Pico

#define LCD_CS 1
#define LCD_MOSI 3
#define LCD_SCK 6

#define SDA 4
#define SCL 5
#define VCN_INT 7

#define CHG_STAT 8
#define CHG_TMR 11

#define PWM_SERVO 15

#define HBR_AH 9
#define HBR_AL 10
#define HBR_BL 12
#define HBR_BH 13

#define ADC_BAT_A A0
#define ADC_BAT_B A1
#define ADC_TEMP_BAT A3

// H-bridge declarations
enum HBR_STATE {OFF, A_POS, B_POS};
int hbrdge_currentState = OFF;

// Proximity Sensor declaration
Adafruit_VCNL4040 vcnl4040 = Adafruit_VCNL4040();
const int proxThreshold = 75; // detectipon threshold for detecting battery in input chute

// LCD declarations
Adafruit_SharpMem display(LCD_SCK, LCD_MOSI, LCD_CS, 400, 240, 8000000);
#define screenWidth 400
#define screenHeight 240
#define BLACK 0
#define WHITE 1

// LVGL declarations
static lv_disp_draw_buf_t draw_buf;
static lv_color_t bufA[ screenWidth * screenHeight / 4 ];
static lv_color_t bufB[ screenWidth * screenHeight / 4 ];
lv_obj_t* objBattPercentage;
lv_obj_t* objBattIcon;
lv_obj_t* panel;
lv_obj_t* ChecksTable;
lv_color_t color_primary = lv_color_hex(0x303030); // gray
lv_obj_t * timeLabel;
lv_obj_t * dateLabel;
lv_obj_t * infoLabel;
lv_obj_t * subInfoLabel;
extern const lv_font_t rubik_140;
lv_obj_t* tabview;
lv_obj_t* dayButton;
lv_obj_t* monthButton;
lv_obj_t* yearButton;
lv_obj_t* weekdayButton;
lv_obj_t* minuteButton;
lv_obj_t* hourButton;
lv_obj_t* dayButtonL;
lv_obj_t* monthButtonL;
lv_obj_t* yearButtonL;
lv_obj_t* weekdayButtonL;
lv_obj_t* minuteButtonL;
lv_obj_t* hourButtonL;
lv_obj_t* formatButton;
lv_obj_t* formatButtonL;
lv_timer_t * returnTimer = NULL;
lv_timer_t * hintTimer = NULL;
bool buttonHintsVisible = true;
lv_obj_t* ejectButton;
lv_obj_t* ejectHint;
lv_obj_t* settingsButton;
lv_obj_t* settingsHint;

Servo servo;
const int LowerServoLimit = 1176; // Servo position for output chute
const int ServoContactPos = 1400; // Servo position for charging
const int UpperServoLimit = 1677; // Servo position for input chute
int currentServoPos = LowerServoLimit;

datetime_t t = {
.year = 2024,
.month = 10,
.day = 26,
.dotw = 6, // 0 is Sunday, so 5 is Friday
.hour = 11,
.min = 42,
.sec = 00
};
datetime_t tOld;
const char* weekdays_GERMAN[7] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
const char* months_GERMAN[13] = {"", "Januar", "Februar", "März", "April", "Mai", "Juni", "Juli", "August", "September", "Oktober", "November", "Dezember"};
const char* weekdays_ENGLISH[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
const char* months_ENGLISH[13] = {"", "January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
boolean hourFormat24 = true;

// State machine declarations
enum FSM_STATE {WAKEUP, IDLE, FEED, CONTACT, CHARGE, ENDCHARGE};
int fsm_currentState = IDLE;

// Display flushing
void my_disp_flush( lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p ){
  uint32_t w = ( area->x2 - area->x1 + 1 );
  uint32_t h = ( area->y2 - area->y1 + 1 );

  int32_t x, y;
    //Lots of room for optimization here
    for(y = area->y1; y <= area->y2; y++) {
        for(x = area->x1; x <= area->x2; x++) {
            display.drawPixel(x, y, lv_color_to16(*color_p));
            color_p++;
        }
    }
  display.refresh();
  lv_disp_flush_ready(disp);
}

// Timer for returning from settings menu to clock screen
static void returnTimer_callback(lv_timer_t * timer)
{
  lv_tabview_set_act(tabview, 0, LV_ANIM_OFF);
}

// Timer for showing the button hints
static void hintTimer_callback(lv_timer_t * timer)
{
  buttonHintsVisible = false;
  lv_obj_add_flag(ejectButton, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(ejectHint, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(settingsHint, LV_OBJ_FLAG_HIDDEN);
}

// Input driver - buttons A and A are assigned to NEXT and ENTER
static void keypad_read(lv_indev_drv_t * indev_drv, lv_indev_data_t * data)
{
  static uint32_t last_key = 0;    

  /*Get the pressed key*/
  uint32_t key = 0;
  if(lv_tabview_get_tab_act(tabview) > 0){ // ignore buttons for tab1
    if(!digitalRead(SW_A) || !digitalRead(SW_B)) lv_timer_reset(returnTimer);
    if(!digitalRead(SW_A)) key = LV_KEY_NEXT;
    else if(!digitalRead(SW_B)) key = LV_KEY_ENTER;

  }
  else{
    if(buttonHintsVisible == true){
      if(!digitalRead(SW_A)) fsm_currentState = ENDCHARGE;
      else if(!digitalRead(SW_B)){ // Enter Settings
        lv_tabview_set_act(tabview, 1, LV_ANIM_OFF);
        lv_group_focus_obj(dayButton);
        // Update button labels with initial RTC date and time
        lv_label_set_text_fmt(dayButtonL, "%02d.", t.day);
        lv_label_set_text_fmt(monthButtonL, "%02d.", t.month);
        lv_label_set_text_fmt(yearButtonL, "%04d", t.year);
        lv_label_set_text_fmt(hourButtonL, "%02d:", t.hour);
        lv_label_set_text_fmt(minuteButtonL, "%02d", t.min);
        // start timer to automatically return to tab1
        lv_timer_ready(returnTimer);
        lv_timer_reset(returnTimer);
      }
    }
    else{ // in tab1
      if(!digitalRead(SW_A) || !digitalRead(SW_B)){
        buttonHintsVisible = true;
        lv_obj_clear_flag(ejectButton, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ejectHint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(settingsHint, LV_OBJ_FLAG_HIDDEN);
        lv_timer_ready(hintTimer);
        lv_timer_reset(hintTimer);
        delay(100);
      }
    }
  }

  // Save the pressed key and set the state
  if(key != 0) {
      data->state = LV_INDEV_STATE_PR;
      last_key = key;
  } else {
      data->state = LV_INDEV_STATE_REL;
  }

  // Set the last pressed key
  data->key = last_key;
}

static void return_button_event_cb(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if(code == LV_EVENT_CLICKED) {
        // Switch to the first tab (index 0) without animation
        lv_tabview_set_act(tabview, 0, LV_ANIM_OFF);
    }
}

static void day_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        t.day = (t.day % 31) + 1; // Cycle through days 1-31
        rtc_set_datetime(&t);
        lv_label_set_text_fmt(dayButtonL, "%02d.", t.day);
    }
}

static void month_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        t.month = (t.month % 12) + 1; // Cycle through months 1-12
        rtc_set_datetime(&t);
        lv_label_set_text_fmt(monthButtonL, "%02d.", t.month);
    }
}

static void year_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        t.year = (t.year < 2034) ? t.year + 1 : 2024; // Cycle through years 2024-2034
        rtc_set_datetime(&t);
        lv_obj_t * btn = lv_event_get_target(e);
        lv_obj_t * label = lv_obj_get_child(btn, 0);
        lv_label_set_text_fmt(label, "%04d", t.year);
    }
}

static void weekday_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        t.dotw = (t.dotw + 1) % 7;
        rtc_set_datetime(&t);
        lv_obj_t * btn = lv_event_get_target(e);
        lv_obj_t * label = lv_obj_get_child(btn, 0);
        lv_label_set_text(label, weekdays_GERMAN[t.dotw]);
    }
}

static void hour_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        t.hour = (t.hour + 1) % 24; // Cycle through hours 0-23
        rtc_set_datetime(&t);
        lv_label_set_text_fmt(hourButtonL, "%02d:", t.hour);
    }
}

static void minute_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        t.min = (t.min + 1) % 60; // Cycle through minutes 0-59
        rtc_set_datetime(&t);
        lv_label_set_text_fmt(minuteButtonL, "%02d", t.min);
    }
}

static void format_button_event_cb(lv_event_t * e)
{
    if(lv_event_get_code(e) == LV_EVENT_CLICKED) {
        hourFormat24 = !hourFormat24;
        if(hourFormat24) lv_label_set_text_fmt(formatButtonL, "24h");
        else lv_label_set_text_fmt(formatButtonL, "12h");
        //Serial.println(hourFormat24);
    }
}

float getVBat(){
  float sum = 0;
  for (int i = 0; i < 16; i++) {
    sum += 0.001 * 2 * (0.806 * analogRead(A1) - 0.806 * analogRead(A0));
  }
  return sum / 16;   
}

// Set the H-bridge state
void h_bridge_set(int hbrState){
  switch (hbrState){
    case A_POS: // Terminal A positive and Terminal B negative
      digitalWrite(HBR_AL, LOW);
      digitalWrite(HBR_AH, HIGH);
      digitalWrite(HBR_BL, HIGH);
      digitalWrite(HBR_BH, LOW);
      break;
    case B_POS: // Terminal B positive and Terminal A negative
      digitalWrite(HBR_AL, HIGH);
      digitalWrite(HBR_AH, LOW);
      digitalWrite(HBR_BL, LOW);
      digitalWrite(HBR_BH, HIGH);
      break;
    case OFF: // All MOSFETs off
    default:
      digitalWrite(HBR_AL, LOW);
      digitalWrite(HBR_AH, LOW);
      digitalWrite(HBR_BL, LOW);
      digitalWrite(HBR_BH, LOW);
  }  
}

// Source: http://www.scynd.de/tutorials/arduino-tutorials/5-sensoren/5-1-temperatur-mit-10k%CF%89-ntc.html
float NTCTemp(pin_size_t ADC_pin, int n_measurements){

  const int ntcNominal = 10000;         // Wiederstand des NTC bei Nominaltemperatur
  const int tempNominal = 25;           // Temperatur bei der der NTC den angegebenen Wiederstand hat
  const int bCoefficient = 3380;        // Beta Coefficient(B25 aus Datenblatt des NTC)

  int series_resistor = 10000;

  int measurements = 0;
  for (int i=0; i < n_measurements; i++)
  {
    measurements += analogRead(ADC_pin);
    delay(10);
  }   
  float average = measurements /= n_measurements;
   
  // Convert the ADC value to a resistance
  average = 4096 / average - 1;
  average = series_resistor / average;
   
  // Umrechnung aller Ergebnisse in die Temperatur mittels einer Steinhard Berechnung
  float temp = average / ntcNominal;     // (R/Ro)
  temp = log(temp);                     // ln(R/Ro)
  temp /= bCoefficient;                 // 1/B * ln(R/Ro)
  temp += 1.0 / (tempNominal + 273.15); // + (1/To)
  temp = 1.0 / temp;                    // Invertieren
  temp -= 273.15;                       // Umwandeln in °C

  return temp;
}

void fsm_idle(){ 
  // keep the servo in the lowest position
  while(currentServoPos != LowerServoLimit){      
    if(currentServoPos < LowerServoLimit) servo.writeMicroseconds(currentServoPos++);
    if(currentServoPos > LowerServoLimit) servo.writeMicroseconds(currentServoPos--);
    delay(3);
  } 
  // Check if a battery is in the feeder chute
  if(vcnl4040.getProximity() > proxThreshold ){
    //battery detected!
    fsm_currentState = FEED;    
  }
}

void fsm_feed(){
  // move the feeder arm up
  while(currentServoPos < UpperServoLimit){      
    servo.writeMicroseconds(currentServoPos++);
    delay(3);
  }
  // Show statis
  lv_label_set_text_fmt(infoLabel, "Loading Cell...");
  lv_timer_handler();
  // wait for the battery to drop into the feeder arm 
  delay(1000);
  fsm_currentState = CONTACT;
}

void fsm_contact(){
  hbrdge_currentState = OFF;
  h_bridge_set(hbrdge_currentState);

  // move the feeder arm to to the middle
  while(currentServoPos > ServoContactPos){      
    servo.writeMicroseconds(currentServoPos--);
    delay(10);
  }
  delay(500);
  // sanity check - proper battery voltage?
  if(abs(getVBat()) < 0.2 || abs(getVBat()) > 1.40){
    fsm_currentState = ENDCHARGE;
  }
  // find out the polarity and set the h-bridge accordingly
  else{
    if(getVBat() >= 0){
      hbrdge_currentState = A_POS;
    }
    else{
      hbrdge_currentState = B_POS;
    }    
    h_bridge_set(hbrdge_currentState);
    // turn off servo while charging
    servo.detach();
    // wait a moment for the charge IC to check the battery
    lv_label_set_text_fmt(infoLabel, "Checking Cell...");
    lv_timer_handler();
    for(int i = 0; i < 3000; i+=100){
      
      delay(100);
    }    
    fsm_currentState = CHARGE;
  }
}

void fsm_charge(){
  boolean chargingOK = true;

  // check if temperature is within limits
  float currentTemperature = NTCTemp(ADC_TEMP_BAT, 5);
  if(currentTemperature > 60 || currentTemperature < 0){
    chargingOK = false;
  }

  // update status label
  switch((millis() / 500) % 4){
    case 0: 
      //lv_label_set_text_fmt(infoLabel, LV_SYMBOL_BATTERY_1);
      lv_label_set_text(infoLabel, LV_SYMBOL_BATTERY_1);
      break;
    case 1: 
      lv_label_set_text(infoLabel, LV_SYMBOL_BATTERY_2);
      break;
    case 2: 
      lv_label_set_text(infoLabel, LV_SYMBOL_BATTERY_3);
      break;
    case 3: 
      lv_label_set_text(infoLabel, LV_SYMBOL_BATTERY_FULL);
      break;
  }
  lv_label_set_text_fmt(subInfoLabel, "%.2fV", abs(getVBat()));
  
  // Detect end of charge or fault condition
  if(digitalRead(CHG_STAT) == HIGH){
    // clear info label
    lv_label_set_text_fmt(infoLabel, "");
    lv_label_set_text_fmt(subInfoLabel, "");
    fsm_currentState = ENDCHARGE;
  }  
}

void fsm_endcharge(){
  // update info label
  lv_label_set_text_fmt(infoLabel, "Ejecting Cell...");
  lv_label_set_text_fmt(subInfoLabel, "");
  lv_timer_handler();
  servo.attach(PWM_SERVO);

  hbrdge_currentState = OFF;
  h_bridge_set(hbrdge_currentState);

  // keep the servo in the lowest position
  while(currentServoPos != LowerServoLimit){      
    if(currentServoPos < LowerServoLimit) servo.writeMicroseconds(currentServoPos++);
    if(currentServoPos > LowerServoLimit) servo.writeMicroseconds(currentServoPos--);
    delay(3);
  }
  delay(5000); // give the DS2712 charger some time to reset

  fsm_currentState = IDLE;

  lv_label_set_text_fmt(infoLabel, "");
  lv_timer_handler();
}

void setup() {

  // --- IO Initialization ---
  pinMode(USER_LED, OUTPUT);

  // Button Pin Definition
  pinMode(SW_B, INPUT_PULLUP);
  pinMode(SW_A, INPUT_PULLUP);

  // Proximity Sensor Interrupt
  pinMode(VCN_INT, INPUT);

  // H-Bridge Controls
  pinMode(HBR_AL, OUTPUT);
  pinMode(HBR_AH, OUTPUT);
  pinMode(HBR_BL, OUTPUT);
  pinMode(HBR_BH, OUTPUT);
  pinMode(ADC_BAT_A, INPUT);
  pinMode(ADC_BAT_B, INPUT);
  pinMode(ADC_TEMP_BAT, INPUT);

  // Charger
  pinMode(CHG_STAT, INPUT_PULLUP);
  pinMode(CHG_TMR, INPUT);
  
  // Servo
  pinMode(PWM_SERVO, OUTPUT);

  //LCD
  pinMode(LCD_CS, OUTPUT); // this line is important since the adafruit library was ported to HW SPI!

  // Serial Init
  Serial.begin(115200);
  
  // Charger Init
  h_bridge_set(OFF);

  // ADC Init
  analogReadResolution(12);

  // RTC Init
  rtc_init();
  rtc_set_datetime(&t);

  // Proximity Sensor Init
  Wire.begin();
  vcnl4040.begin();

  // Display Init
  display.begin();
  display.setRotation(0);
  display.clearDisplay();
  display.refresh();

  h_bridge_set(hbrdge_currentState);

  servo.writeMicroseconds(currentServoPos);
  servo.attach(PWM_SERVO);

  // Setup LVGL
  lv_init();
  lv_disp_draw_buf_init( &draw_buf, bufA, bufB, screenWidth * screenHeight / 4 );

  // Initialize the display driver
  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init( &disp_drv );
  disp_drv.hor_res = screenWidth;
  disp_drv.ver_res = screenHeight;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register( &disp_drv );

  // Initialize the keyboard driver
  static lv_indev_drv_t indev_drv;    
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_KEYPAD;
  indev_drv.read_cb = keypad_read;
  lv_indev_t *indev = lv_indev_drv_register(&indev_drv);

  // --- LVGL UI Configuration ---  
  
  // Set the background color
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), LV_PART_MAIN);

  tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 0); // Hide tab labels by setting their height to 0
  lv_obj_set_style_bg_color(tabview, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_size(tabview, screenWidth, 240);
  lv_obj_align(tabview, LV_ALIGN_TOP_MID, 0, 0);

  // Add tabs to tabview
  lv_obj_t* tab1 = lv_tabview_add_tab(tabview, "Clock");
  lv_obj_t* tab2 = lv_tabview_add_tab(tabview, "Settings");

  // Creat labels for date, time and status

  timeLabel = lv_label_create(tab1);
  lv_label_set_text(timeLabel, "12:35");
  lv_obj_set_style_text_color(timeLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(timeLabel, &rubik_140, LV_PART_MAIN);
  lv_obj_center(timeLabel);

  dateLabel = lv_label_create(tab1);
  lv_label_set_text(dateLabel, "Samstag, 14. September");
  lv_obj_set_style_text_color(dateLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(dateLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_center(dateLabel);
  lv_obj_set_pos(dateLabel, 0, -80);

  infoLabel = lv_label_create(tab1);
  lv_label_set_text(infoLabel, "");
  lv_obj_set_style_text_color(infoLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(infoLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_center(infoLabel);
  lv_obj_set_pos(infoLabel, 0, 74);

  subInfoLabel = lv_label_create(tab1);
  lv_label_set_text(subInfoLabel, "");
  lv_obj_set_style_text_color(subInfoLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(subInfoLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_center(subInfoLabel);
  lv_obj_set_pos(subInfoLabel, 0, 100);

  // Eject Button

  ejectButton = lv_btn_create(tab1);
  lv_obj_t* ejectButtonL = lv_label_create(ejectButton);
  lv_obj_align(ejectButton, LV_ALIGN_BOTTOM_LEFT, 0, 0);
  lv_obj_set_size(ejectButton, 50, 30);
  lv_obj_set_style_bg_color(ejectButton, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_color(ejectButton, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_width(ejectButton, 2, LV_PART_MAIN);
  lv_label_set_text(ejectButtonL, LV_SYMBOL_EJECT);
  lv_obj_set_style_text_font(ejectButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(ejectButtonL, LV_ALIGN_CENTER, 0, 0);
  ejectHint = lv_label_create(tab1);
  lv_label_set_text(ejectHint, "A");
  lv_obj_set_style_text_font(ejectHint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(ejectHint, LV_ALIGN_BOTTOM_LEFT, 60, -6);

  // Settings Button

  settingsButton = lv_btn_create(tab1);
  lv_obj_t* settingsButtonL = lv_label_create(settingsButton);
  lv_obj_align(settingsButton, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
  lv_obj_set_size(settingsButton, 50, 30);
  lv_obj_set_style_bg_color(settingsButton, lv_color_black(), LV_PART_MAIN);
  lv_obj_set_style_border_color(settingsButton, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_border_width(settingsButton, 2, LV_PART_MAIN);
  lv_label_set_text(settingsButtonL, LV_SYMBOL_SETTINGS);
  lv_obj_set_style_text_font(settingsButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(settingsButtonL, LV_ALIGN_CENTER, 0, 0);
  settingsHint = lv_label_create(tab1);
  lv_label_set_text(settingsHint, "B");
  lv_obj_set_style_text_font(settingsHint, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(settingsHint, LV_ALIGN_BOTTOM_RIGHT, -60, -6);

  
  // --------------------------------------------------------------------------------
  // Content for tab2

  lv_obj_t* settingsLabel = lv_label_create(tab2);
  lv_label_set_text(settingsLabel, "Settings");
  lv_obj_set_style_text_color(settingsLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(settingsLabel, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_center(settingsLabel);
  lv_obj_align(settingsLabel, LV_ALIGN_TOP_LEFT, 0, 0);

  // Date Setting

  lv_obj_t* tempLabel = lv_label_create(tab2);
  lv_label_set_text(tempLabel, "Date:");
  lv_obj_set_style_text_color(tempLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(tempLabel, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_center(tempLabel);
  lv_obj_align(tempLabel, LV_ALIGN_TOP_LEFT, 0, 50);

  dayButton = lv_btn_create(tab2);
  dayButtonL = lv_label_create(dayButton);
  lv_obj_align(dayButton, LV_ALIGN_TOP_LEFT, 50, 44);
  lv_obj_set_size(dayButton, 40, 30);  
  lv_obj_set_style_bg_color(dayButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(dayButtonL, "26.");
  lv_obj_set_style_text_font(dayButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(dayButtonL, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_outline_color(dayButton, lv_color_white(), LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_opa(dayButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(dayButton, day_button_event_cb, LV_EVENT_CLICKED, NULL);

  monthButton = lv_btn_create(tab2);
  monthButtonL = lv_label_create(monthButton);
  lv_obj_align(monthButton, LV_ALIGN_TOP_LEFT, 94, 44);
  lv_obj_set_size(monthButton, 40, 30);
  lv_obj_set_style_bg_color(monthButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(monthButtonL, "10.");
  lv_obj_set_style_text_font(monthButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(monthButtonL, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_outline_color(monthButton, lv_color_white(), LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_opa(monthButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(monthButton, month_button_event_cb, LV_EVENT_CLICKED, NULL);

  yearButton = lv_btn_create(tab2);
  yearButtonL = lv_label_create(yearButton);
  lv_obj_align(yearButton, LV_ALIGN_TOP_LEFT, 138, 44);
  lv_obj_set_size(yearButton, 60, 30);
  lv_obj_set_style_bg_color(yearButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(yearButtonL, "2024");
  lv_obj_set_style_text_font(yearButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(yearButtonL, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_outline_color(yearButton, lv_color_white(), LV_STATE_FOCUS_KEY);
  lv_obj_set_style_outline_opa(yearButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(yearButton, year_button_event_cb, LV_EVENT_CLICKED, NULL);
  
  weekdayButton = lv_btn_create(tab2);
  weekdayButtonL = lv_label_create(weekdayButton);
  lv_obj_align(weekdayButton, LV_ALIGN_TOP_LEFT, 202, 44);
  lv_obj_set_size(weekdayButton, 104, 30);
  lv_obj_set_style_bg_color(weekdayButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(weekdayButtonL, weekdays_GERMAN[t.dotw]);
  lv_obj_set_style_text_font(weekdayButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(weekdayButtonL, LV_ALIGN_CENTER, 0, 0);  
  lv_obj_set_style_outline_opa(weekdayButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(weekdayButton, weekday_button_event_cb, LV_EVENT_CLICKED, NULL);


  // Time Setting

  tempLabel = lv_label_create(tab2);
  lv_label_set_text(tempLabel, "Time:");
  lv_obj_set_style_text_color(tempLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(tempLabel, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_center(tempLabel);
  lv_obj_align(tempLabel, LV_ALIGN_TOP_LEFT, 0, 84);

  hourButton = lv_btn_create(tab2);
  hourButtonL = lv_label_create(hourButton);
  lv_obj_align(hourButton, LV_ALIGN_TOP_LEFT, 50, 78);
  lv_obj_set_size(hourButton, 40, 30);
  lv_obj_set_style_bg_color(hourButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(hourButtonL, "35:");
  lv_obj_set_style_text_font(hourButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(hourButtonL, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_outline_opa(hourButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(hourButton, hour_button_event_cb, LV_EVENT_CLICKED, NULL);

  minuteButton = lv_btn_create(tab2);
  minuteButtonL = lv_label_create(minuteButton);
  lv_obj_align(minuteButton, LV_ALIGN_TOP_LEFT, 94, 78);
  lv_obj_set_size(minuteButton, 36, 30);
  lv_obj_set_style_bg_color(minuteButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(minuteButtonL, "12");
  lv_obj_set_style_text_font(minuteButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(minuteButtonL, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_outline_opa(minuteButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(minuteButton, minute_button_event_cb, LV_EVENT_CLICKED, NULL);

  // Hour Format Setting

  tempLabel = lv_label_create(tab2);
  lv_label_set_text(tempLabel, "Format:");
  lv_obj_set_style_text_color(tempLabel, lv_color_white(), LV_PART_MAIN);
  lv_obj_set_style_text_font(tempLabel, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_center(tempLabel);
  lv_obj_align(tempLabel, LV_ALIGN_TOP_LEFT, 0, 118);

  formatButton = lv_btn_create(tab2);
  formatButtonL = lv_label_create(formatButton);
  lv_obj_align(formatButton, LV_ALIGN_TOP_LEFT, 70, 112);
  lv_obj_set_size(formatButton, 50, 30);
  lv_obj_set_style_bg_color(formatButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(formatButtonL, "24h");
  lv_obj_set_style_text_font(formatButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(formatButtonL, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_outline_opa(formatButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(formatButton, format_button_event_cb, LV_EVENT_CLICKED, NULL);

  // Return Button

  lv_obj_t* returnButton = lv_btn_create(tab2);
  lv_obj_t* returnButtonL = lv_label_create(returnButton);
  lv_obj_align(returnButton, LV_ALIGN_BOTTOM_LEFT, 0, 00);
  lv_obj_set_size(returnButton, 84, 30);
  lv_obj_set_style_bg_color(returnButton, lv_color_black(), LV_PART_MAIN);
  lv_label_set_text(returnButtonL, LV_SYMBOL_NEW_LINE " Return");
  lv_obj_set_style_text_font(returnButtonL, &lv_font_montserrat_16, LV_PART_MAIN);
  lv_obj_align(returnButtonL, LV_ALIGN_CENTER, 0, 0); 
  lv_obj_set_style_outline_opa(returnButton, 255, LV_STATE_FOCUS_KEY);
  lv_obj_add_event_cb(returnButton, return_button_event_cb, LV_EVENT_CLICKED, NULL);

  lv_gridnav_add(tab2, LV_GRIDNAV_CTRL_ROLLOVER);

  /* Create a group and add objects to it */ 
  lv_group_t * group = lv_group_create();
  lv_group_add_obj(group, dayButton);
  lv_group_add_obj(group, monthButton);
  lv_group_add_obj(group, yearButton);
  lv_group_add_obj(group, weekdayButton);
  lv_group_add_obj(group, hourButton);
  lv_group_add_obj(group, minuteButton);
  lv_group_add_obj(group, formatButton);
  lv_group_add_obj(group, returnButton);
  /* Assign the input device to the group */
  lv_indev_set_group(indev, group);
  

  lv_group_focus_obj(dayButton);

  // Timer for returning from settings menu to clock
  returnTimer = lv_timer_create(returnTimer_callback, 10000, NULL);
  // Timer for displaying button hints
  hintTimer = lv_timer_create(hintTimer_callback, 3000, NULL);

  lv_tabview_set_act(tabview, 0, LV_ANIM_OFF);  
}


void loop() {
  // Blink debug LED at 1 Hz
  digitalWrite(USER_LED, millis() % 1000 > 500);
  
  #ifdef DEBUGDISPLAY 
  
    display.setTextColor(BLACK);
    display.setTextSize(2);
    display.setCursor(0, 10);  

    display.fillScreen(WHITE);

    display.print("H-Bridge Satus: ");
    if(hbrdge_currentState == OFF) display.println("OFF");
    if(hbrdge_currentState == A_POS) display.println("A+ B-");
    if(hbrdge_currentState == B_POS) display.println("A- B+");  

    display.print("ADC_TEMP_BAT: ");
    display.print(NTCTemp(ADC_TEMP_BAT, 5));
    display.println("C");

    display.print("V_BAT: ");
    display.print(getVBat());
    display.println("V");

    display.print("Buttons pressed:");
    if(button_L.isPressed()) display.print("L ");
    if(button_R.isPressed()) display.print("R");
    display.println();

    display.print("Charge status: ");
    if(digitalRead(CHG_STAT)) display.println("complete/disabled");
    else display.println("charging");

    display.print("Proximity: ");
    display.print("-");
    display.print(vcnl4040.getProximity());
    display.println();

    display.print("Servo Position: ");
    display.print(currentServoPos);
    display.println();

    display.print("State Machine is at: ");
    display.print(fsm_currentState);
    display.println();

  #endif
  
  // Update servo position
  currentServoPos = constrain(currentServoPos, LowerServoLimit, UpperServoLimit);
  servo.writeMicroseconds(currentServoPos);
  
  // Handle the state machine
  switch(fsm_currentState){
  case IDLE:
    fsm_idle();
    break;
  case FEED:
    fsm_feed();
    break;
  case CONTACT:
    fsm_contact();
    break;
  case CHARGE:
    fsm_charge();
    break;
  case ENDCHARGE:
    fsm_endcharge();
    break;
  }

  // Update the clock display
  rtc_get_datetime(&t);
  if(tOld.min != t.min || tOld.hour != t.hour){ // avoid updating the time label too often (increases FPS)
    if(!hourFormat24 && t.hour > 12) lv_label_set_text_fmt(timeLabel, "%d:%02d", t.hour-12, t.min);
    else lv_label_set_text_fmt(timeLabel, "%d:%02d", t.hour, t.min);
  }
  if(tOld.day != t.day || tOld.month != t.month || tOld.dotw != t.dotw){ // avoid updating the date label too often (increases FPS)
    lv_label_set_text_fmt(dateLabel, "%s, %d. %s", weekdays_GERMAN[t.dotw], t.day, months_GERMAN[t.month]);
  }
  tOld = t;

  
  // Update LVGL UI
  lv_timer_handler();
}