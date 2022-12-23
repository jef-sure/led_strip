#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/rmt.h"
#include "led_strip.h"

#include <time.h>
#include <stdlib.h>

static const char *TAG = "fire strip";
enum {
    LedStripNumber = 360
};

enum {
    FireStripProgram = 0, RainbowProgram = 1, LastProgram = RainbowProgram
};

enum {
    StateOn = 1, FadingOut = 2, FadingIn = 3
};

int CurrentProgram = FireStripProgram;
int NextProgram = FireStripProgram;
int ProgramState = StateOn;
int NormalLightness = 64;
int CurrentLightness = 64;

QueueHandle_t xQueEvent;

#define BUTTON_PROGRAM 23
#define BUTTON_PALITRA 22
#define BUTTON_ONOFF 19

#define RMT_TX_CHANNEL RMT_CHANNEL_0
#define RMT_TX_GPIO 14

#define PALITRA_NUM 64
#define _R 0
#define _G 1
#define _B 2

uint8_t Palitra[PALITRA_NUM][3];
int CurrentPalitra = 0;
int CurrentLength = 0;
int IsOn = 1;

uint8_t RainbowStrip[LedStripNumber][3];
uint8_t FireStrip[LedStripNumber][3];
uint8_t Lightness[LedStripNumber];

void makePalitra(int palno) {
    uint8_t pals[6][2] = { { 0, 1 }, { 0, 2 }, { 1, 0 }, { 1, 2 }, { 2, 0 }, { 2, 1 } };
    int first = pals[palno % 6][0];
    int second = pals[palno % 6][1];
    int third = 3 - second - first;
    for (int i = 0; i < PALITRA_NUM / 2; i++) {
        Palitra[i][first] = i * (512 / PALITRA_NUM);
        Palitra[i][second] = 0;
        Palitra[i][third] = 0;
    }
    for (int i = PALITRA_NUM / 2; i < PALITRA_NUM; i++) {
        Palitra[i][first] = 255;
        Palitra[i][second] = (i - PALITRA_NUM / 2) * (512 / PALITRA_NUM);
        Palitra[i][third] = 0;
    }
}

#define FIRE_LINES 10
#define FIRE_SHIFT 1
#define FIRE_COLS LedStripNumber + FIRE_SHIFT * 2

uint8_t FireScreen[2][FIRE_LINES][FIRE_COLS];
int currentFireScreen = 0;

void drawFire() {
    int nextScreen = currentFireScreen ^ 1;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < FIRE_COLS; i += 2) {
            FireScreen[nextScreen][j][i + 1] = FireScreen[nextScreen][j][i] = FireScreen[currentFireScreen][j][i + 1] =
                    FireScreen[currentFireScreen][j][i] = rand() < RAND_MAX / 2 ? 0 : PALITRA_NUM - 1;
        }
    }
    for (int j = 1; j < FIRE_LINES - 1; j++) {
        for (int i = FIRE_SHIFT; i < FIRE_COLS - FIRE_SHIFT; i++) {
            int mid = (FireScreen[currentFireScreen][j][i - 1] + FireScreen[currentFireScreen][j][i + 1]
                    + FireScreen[currentFireScreen][j][i] + FireScreen[currentFireScreen][j - 1][i]) / 4;
            if (mid > 0) mid--;
            FireScreen[nextScreen][j + 1][i] = mid;
        }
    }
    currentFireScreen ^= 1;
    for (int i = 0; i < LedStripNumber; i++) {
        FireStrip[i][_R] = Palitra[FireScreen[currentFireScreen][FIRE_LINES - 1][i + FIRE_SHIFT]][_R];
        FireStrip[i][_G] = Palitra[FireScreen[currentFireScreen][FIRE_LINES - 1][i + FIRE_SHIFT]][_G];
        FireStrip[i][_B] = Palitra[FireScreen[currentFireScreen][FIRE_LINES - 1][i + FIRE_SHIFT]][_B];
    }
}

void RainbowCirclePoint(int idxGrad, uint8_t *r, uint8_t *g, uint8_t *b) {
    idxGrad %= 360;
    if (idxGrad < 0) idxGrad += 360;
    if (idxGrad < 120) {
        *b = 0;
        *g = idxGrad * 255 / 119;
        *r = 255 - *g;
    } else if (idxGrad < 240) {
        *r = 0;
        *b = (idxGrad - 120) * 255 / 119;
        *g = 255 - *b;
    } else {
        *g = 0;
        *r = (idxGrad - 240) * 255 / 119;
        *b = 255 - *r;
    }
}

void vTaskGetFire(void *pvParameters) {
    static int message = FireStripProgram;
    int count = 0;
    for (;;) {
        if (CurrentProgram == FireStripProgram) {
            drawFire();
            if (count > FIRE_LINES * 2) {
                count = FIRE_LINES * 2 + 1;
                xQueueSendToFront(xQueEvent, &message, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            } else {
                ++count;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void vTaskGetRainbow(void *pvParameters) {
    static int message = RainbowProgram;
    int startAngle = 0;
    for (;;) {
        if (CurrentProgram == RainbowProgram) {
            for (int i = 0; i < LedStripNumber; ++i) {
                uint8_t r;
                uint8_t g;
                uint8_t b;
                RainbowCirclePoint(startAngle + i, &r, &g, &b);
                RainbowStrip[i][_R] = r;
                RainbowStrip[i][_G] = g;
                RainbowStrip[i][_B] = b;
            }
            xQueueSendToFront(xQueEvent, &message, 0);
            startAngle = (startAngle + 1) % 360;
            vTaskDelay(pdMS_TO_TICKS(100));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void vTaskGetButtons(void *pvParameters) {
    int OldpalLevel = gpio_get_level(BUTTON_PALITRA);
    int OldIsOnLevel = gpio_get_level(BUTTON_ONOFF);
    int OldProgramLevel = gpio_get_level(BUTTON_PROGRAM);
    for (;;) {
        int palLevel = gpio_get_level(BUTTON_PALITRA);
        if (palLevel == 0 && palLevel != OldpalLevel) {
            makePalitra(++CurrentPalitra);
        }
        int onoffLevel = gpio_get_level(BUTTON_ONOFF);
        if (onoffLevel == 0 && onoffLevel != OldIsOnLevel) {
            IsOn ^= 1;
        }
        int programLevel = gpio_get_level(BUTTON_PROGRAM);
        if (programLevel == 0 && programLevel != OldProgramLevel) {
            if (ProgramState == StateOn) {
                ProgramState = FadingOut;
            }
            NextProgram = (NextProgram + 1) % (LastProgram + 1);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        OldpalLevel = palLevel;
        OldIsOnLevel = onoffLevel;
        OldProgramLevel = programLevel;
    }
}

void app_main(void) {
    makePalitra(CurrentPalitra);
    gpio_pad_select_gpio(BUTTON_PALITRA);
    gpio_set_direction(BUTTON_PALITRA, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_PALITRA);
    gpio_pad_select_gpio(BUTTON_PROGRAM);
    gpio_set_direction(BUTTON_PROGRAM, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_PROGRAM);
    gpio_pad_select_gpio(BUTTON_ONOFF);
    gpio_set_direction(BUTTON_ONOFF, GPIO_MODE_INPUT);
    gpio_pullup_en(BUTTON_ONOFF);
    xQueEvent = xQueueCreate(20, sizeof(int));

    xTaskCreate(vTaskGetButtons, "Buttons", 4096, 0, tskIDLE_PRIORITY + 1, 0);
    xTaskCreate(vTaskGetFire, "GetFire", 4096, 0, tskIDLE_PRIORITY + 1, 0);
    xTaskCreate(vTaskGetRainbow, "GetRainbow", 4096, 0, tskIDLE_PRIORITY + 1, 0);

    rmt_config_t config = RMT_DEFAULT_CONFIG_TX(RMT_TX_GPIO, RMT_TX_CHANNEL);
    // set counter clock to 40MHz
    config.clk_div = 2;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));

    // install ws2812 driver
    led_strip_config_t strip_config = LED_STRIP_DEFAULT_CONFIG(LedStripNumber, (led_strip_dev_t) config.channel);
    led_strip_t *strip = led_strip_new_rmt_ws2812(&strip_config);
    if (!strip) {
        ESP_LOGE(TAG, "install WS2812 driver failed");
    }
    // Clear LED strip (turn off all LEDs)
    strip->clear(strip, 100);
    for (int i = 0; i < LedStripNumber; ++i) {
        Lightness[i] = NormalLightness;
    }
    CurrentLength = 0;
    int message;
    while (true) {
        if (xQueueReceive(xQueEvent, &message, pdMS_TO_TICKS(10))) {
//            printf("Received: %d\n", message);
        }
        if (IsOn == 1) {
            if (CurrentLength < LedStripNumber) CurrentLength++;
        } else {
            if (CurrentLength > 0) CurrentLength--;
        }
        if (ProgramState == FadingOut) {
            if (CurrentLightness > 0) {
                CurrentLightness--;
                for (int i = 0; i < LedStripNumber; ++i) {
                    if (Lightness[i] > 0) {
                        Lightness[i] = Lightness[i] - 1;
                    }
                }
            } else {
                ProgramState = FadingIn;
            }
        } else if (ProgramState == FadingIn) {
            CurrentProgram = NextProgram;
            if (CurrentLightness < NormalLightness) {
                CurrentLightness++;
                for (int i = 0; i < LedStripNumber; ++i) {
                    if (Lightness[i] < NormalLightness) {
                        Lightness[i] = Lightness[i] + 1;
                    }
                }
            } else {
                ProgramState = StateOn;
            }
        }
        for (int i = 0; i < CurrentLength; i++) {
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            if (CurrentProgram == FireStripProgram) {
                red = FireStrip[i][_R];
                green = FireStrip[i][_G];
                blue = FireStrip[i][_B];
            } else if (CurrentProgram == RainbowProgram) {
                red = RainbowStrip[i][_R];
                green = RainbowStrip[i][_G];
                blue = RainbowStrip[i][_B];
            }
            red = (uint8_t) ((int16_t) red * (int16_t) Lightness[i] / 255);
            green = (uint8_t) ((int16_t) green * (int16_t) Lightness[i] / 255);
            blue = (uint8_t) ((int16_t) blue * (int16_t) Lightness[i] / 255);
//            printf("Set: %d = (%d, %d, %d)\n", i, red, green, blue);
            strip->set_pixel(strip, i, red, green, blue);
        }
        for (int i = CurrentLength; i < LedStripNumber; i++) {
            strip->set_pixel(strip, i, 0, 0, 0);
        }
        strip->refresh(strip, 100);
    }
}
