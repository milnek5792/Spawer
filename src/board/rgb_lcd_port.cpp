/*****************************************************************************
 * rgb_lcd_port.cpp — Waveshare ESP32-S3-Touch-LCD-7B (oficiální timing)
 ******************************************************************************/

#include "rgb_lcd_port.h"
#include <string.h>

static const char *TAG = "rgb_lcd_port";
static esp_lcd_panel_handle_t panel_handle = NULL;

esp_lcd_panel_handle_t waveshare_esp32_s3_rgb_lcd_init()
{
    ESP_LOGI(TAG, "Install RGB LCD %dx%d pclk=%u fbs=%u bounce=%u",
             EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES,
             (unsigned)EXAMPLE_LCD_PIXEL_CLOCK_HZ,
             (unsigned)EXAMPLE_LCD_RGB_BUFFER_NUMS,
             (unsigned)EXAMPLE_RGB_BOUNCE_BUFFER_SIZE);

    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_config.timings.pclk_hz = EXAMPLE_LCD_PIXEL_CLOCK_HZ;
    panel_config.timings.h_res = EXAMPLE_LCD_H_RES;
    panel_config.timings.v_res = EXAMPLE_LCD_V_RES;
    panel_config.timings.hsync_pulse_width = 162;
    panel_config.timings.hsync_back_porch = 152;
    panel_config.timings.hsync_front_porch = 48;
    panel_config.timings.vsync_pulse_width = 45;
    panel_config.timings.vsync_back_porch = 13;
    panel_config.timings.vsync_front_porch = 3;
    panel_config.timings.flags.pclk_active_neg = 1;

    panel_config.data_width = EXAMPLE_RGB_DATA_WIDTH;
    panel_config.bits_per_pixel = EXAMPLE_RGB_BIT_PER_PIXEL;
    panel_config.num_fbs = EXAMPLE_LCD_RGB_BUFFER_NUMS;
    panel_config.bounce_buffer_size_px = EXAMPLE_RGB_BOUNCE_BUFFER_SIZE;
    panel_config.dma_burst_size = 64;

    panel_config.hsync_gpio_num = EXAMPLE_LCD_IO_RGB_HSYNC;
    panel_config.vsync_gpio_num = EXAMPLE_LCD_IO_RGB_VSYNC;
    panel_config.de_gpio_num = EXAMPLE_LCD_IO_RGB_DE;
    panel_config.pclk_gpio_num = EXAMPLE_LCD_IO_RGB_PCLK;
    panel_config.disp_gpio_num = EXAMPLE_LCD_IO_RGB_DISP;
    panel_config.data_gpio_nums[0] = EXAMPLE_LCD_IO_RGB_DATA0;
    panel_config.data_gpio_nums[1] = EXAMPLE_LCD_IO_RGB_DATA1;
    panel_config.data_gpio_nums[2] = EXAMPLE_LCD_IO_RGB_DATA2;
    panel_config.data_gpio_nums[3] = EXAMPLE_LCD_IO_RGB_DATA3;
    panel_config.data_gpio_nums[4] = EXAMPLE_LCD_IO_RGB_DATA4;
    panel_config.data_gpio_nums[5] = EXAMPLE_LCD_IO_RGB_DATA5;
    panel_config.data_gpio_nums[6] = EXAMPLE_LCD_IO_RGB_DATA6;
    panel_config.data_gpio_nums[7] = EXAMPLE_LCD_IO_RGB_DATA7;
    panel_config.data_gpio_nums[8] = EXAMPLE_LCD_IO_RGB_DATA8;
    panel_config.data_gpio_nums[9] = EXAMPLE_LCD_IO_RGB_DATA9;
    panel_config.data_gpio_nums[10] = EXAMPLE_LCD_IO_RGB_DATA10;
    panel_config.data_gpio_nums[11] = EXAMPLE_LCD_IO_RGB_DATA11;
    panel_config.data_gpio_nums[12] = EXAMPLE_LCD_IO_RGB_DATA12;
    panel_config.data_gpio_nums[13] = EXAMPLE_LCD_IO_RGB_DATA13;
    panel_config.data_gpio_nums[14] = EXAMPLE_LCD_IO_RGB_DATA14;
    panel_config.data_gpio_nums[15] = EXAMPLE_LCD_IO_RGB_DATA15;
    panel_config.flags.fb_in_psram = 1;

    esp_err_t err = esp_lcd_new_rgb_panel(&panel_config, &panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_rgb_panel: %s", esp_err_to_name(err));
        return NULL;
    }
    err = esp_lcd_panel_init(panel_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_panel_init: %s", esp_err_to_name(err));
        return NULL;
    }
    return panel_handle;
}

void wavesahre_rgb_lcd_display_window(int16_t Xstart, int16_t Ystart, int16_t Xend, int16_t Yend, uint8_t *Image)
{
    if (!panel_handle || !Image) {
        return;
    }
    if (Xstart < 0) {
        Xstart = 0;
    }
    if (Ystart < 0) {
        Ystart = 0;
    }
    if (Xend > EXAMPLE_LCD_H_RES) {
        Xend = EXAMPLE_LCD_H_RES;
    }
    if (Yend > EXAMPLE_LCD_V_RES) {
        Yend = EXAMPLE_LCD_V_RES;
    }
    esp_lcd_panel_draw_bitmap(panel_handle, Xstart, Ystart, Xend, Yend, Image);
}

void wavesahre_rgb_lcd_display(uint8_t *Image)
{
    if (!panel_handle || !Image) {
        return;
    }
    esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, EXAMPLE_LCD_H_RES, EXAMPLE_LCD_V_RES, Image);
}

void waveshare_get_frame_buffer(void **buf1, void **buf2)
{
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, buf1, buf2));
}

static uint8_t s_blPercent = 60;

void waveshare_rgb_lcd_set_brightness(uint8_t percent)
{
    if (percent > 97) {
        percent = 97;
    }
    s_blPercent = percent;
    if (percent == 0) {
        IO_EXTENSION_Output(IO_EXTENSION_IO_2, 0);
        return;
    }
    // CH32V003 na 7B: vyšší PWM = tmavší → invertovat
    uint8_t pwm = static_cast<uint8_t>(100 - percent);
    if (pwm < 3) {
        pwm = 3;
    }
    if (pwm > 97) {
        pwm = 97;
    }
    // Nejdřív PWM, pak enable — jinak po sleep může zůstat tma
    IO_EXTENSION_Pwm_Output(pwm);
    IO_EXTENSION_Output(IO_EXTENSION_IO_2, 1);
}

uint8_t waveshare_rgb_lcd_get_brightness(void)
{
    return s_blPercent;
}

void wavesahre_rgb_lcd_bl_on()
{
    waveshare_rgb_lcd_set_brightness(s_blPercent ? s_blPercent : 60);
}

void wavesahre_rgb_lcd_bl_off()
{
    // Jen vypnout enable — PWM 0 by při invertované polaritě rozsvítilo
    IO_EXTENSION_Output(IO_EXTENSION_IO_2, 0);
}
