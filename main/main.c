/*--------------------------------------------------------------------------------------------------------------------*
 |                                    Digital Dryad growbox main application file                                     |
 |                                             Copyright 2026 Jacob Beck                                              |
 *--------------------------------------------------------------------------------------------------------------------*/

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_check.h>
#include <esp_err.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>
#include <lvgl.h>

#include <esp_lcd_ili9488.h>
#include <esp_lcd_touch_xpt2046.h>

// Log tag
static const char *TAG = "MAIN";

// LCD size
static const int LCD_HORIZONTAL_PIXELS = 320;
// static const int LCD_VERTICAL_PIXELS   = 480;

// LCD settings
static const int          LCD_COMMAND_BITS      = 8;
static const int          LCD_PARAMETER_BITS    = 8;
static const int          LCD_BITS_PER_PIXEL    = 18;
static const unsigned int LCD_CLOCK_HZ        = 40e6; // Reduce bus speed if display artifacts appear
static const int          LCD_SPI_QUEUE_LEN     = 10;
static const int          SPI_MAX_TRANSFER_SIZE = 1 << 15; // 32768 bytes

static const esp_lcd_spi_bus_handle_t LCD_SPI_BUS    = SPI2_HOST;
static const lcd_rgb_element_order_t  LCD_COLOR_MODE = COLOR_RGB_ELEMENT_ORDER_BGR;

// Default to 25 lines of color data
static const size_t LV_BUFFER_SIZE        = LCD_HORIZONTAL_PIXELS * 25;
// static const int    LVGL_UPDATE_PERIOD_MS = 5;

// LCD backlight
static const ledc_mode_t      BACKLIGHT_LEDC_MODE             = LEDC_LOW_SPEED_MODE;
static const ledc_channel_t   BACKLIGHT_LEDC_CHANNEL          = LEDC_CHANNEL_0;
static const ledc_timer_t     BACKLIGHT_LEDC_TIMER            = LEDC_TIMER_1;
static const ledc_timer_bit_t BACKLIGHT_LEDC_TIMER_RESOLUTION = LEDC_TIMER_10_BIT;
static const uint32_t         BACKLIGHT_LEDC_FRQUENCY         = 5000;

static esp_lcd_panel_io_handle_t lcd_io       = NULL;
static esp_lcd_panel_handle_t    lcd_handle   = NULL;
// static esp_lcd_touch_handle_t    touch_handle = NULL;

// static lv_disp_t  *lv_display = NULL;
// static lv_color_t *lv_buf_1   = NULL;
// static lv_color_t *lv_buf_2   = NULL;
// static lv_obj_t   *meter      = NULL;
// static lv_style_t  style_screen;

#define EXAMPLE_LCD_DRAW_BUFF_DOUBLE (1)
#define EXAMPLE_LCD_DRAW_BUFF_HEIGHT (50)
#define EXAMPLE_LCD_BL_ON_LEVEL      (1)

// LCD pins (ESP32-S3)
static const gpio_num_t SPI_CLOCK     = GPIO_NUM_11;
static const gpio_num_t SPI_MOSI      = GPIO_NUM_10;
static const gpio_num_t SPI_MISO      = GPIO_NUM_13;
static const gpio_num_t LCD_CS        = GPIO_NUM_3;
static const gpio_num_t LCD_RESET     = GPIO_NUM_46;
static const gpio_num_t LCD_DC        = GPIO_NUM_9;
static const gpio_num_t LCD_BACKLIGHT = GPIO_NUM_12;

/* Touch settings */
#define EXAMPLE_TOUCH_I2C_NUM    (0)
#define EXAMPLE_TOUCH_I2C_CLK_HZ (400000)

/* LCD touch pins */
#define EXAMPLE_TOUCH_I2C_SCL  (GPIO_NUM_18)
#define EXAMPLE_TOUCH_I2C_SDA  (GPIO_NUM_8)
#define EXAMPLE_TOUCH_GPIO_INT (GPIO_NUM_3)

// LVGL image declare
LV_IMG_DECLARE(esp_logo)

/* LVGL display and touch */
// static lv_display_t *lvgl_disp        = NULL;
// static lv_indev_t   *lvgl_touch_indev = NULL;

static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    // LCD backlight
    ESP_LOGI(TAG, "Initializing LEDC for backlight pin: %d", LCD_BACKLIGHT);

    const ledc_timer_config_t lcd_backlight_timer = {
        .speed_mode      = BACKLIGHT_LEDC_MODE,
        .duty_resolution = BACKLIGHT_LEDC_TIMER_RESOLUTION,
        .timer_num       = BACKLIGHT_LEDC_TIMER,
        .freq_hz         = BACKLIGHT_LEDC_FRQUENCY,
        .clk_cfg         = LEDC_AUTO_CLK,
    };

    const ledc_channel_config_t lcd_backlight_channel = {
        .gpio_num   = LCD_BACKLIGHT,
        .speed_mode = BACKLIGHT_LEDC_MODE,
        .channel    = BACKLIGHT_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BACKLIGHT_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
        .flags      = { .output_invert = 0 },
    };
    ESP_ERROR_CHECK(ledc_timer_config(&lcd_backlight_timer));
    ESP_ERROR_CHECK(ledc_channel_config(&lcd_backlight_channel));

    ESP_ERROR_CHECK(ledc_set_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL, 1023));
    ESP_ERROR_CHECK(ledc_update_duty(BACKLIGHT_LEDC_MODE, BACKLIGHT_LEDC_CHANNEL));

    // SPI bus
    ESP_LOGI(TAG, "Initializing SPI bus (MOSI:%d, MISO:%d, CLK:%d)", SPI_MOSI, SPI_MISO, SPI_CLOCK);

    spi_bus_config_t bus = {
        .mosi_io_num     = SPI_MOSI,
        .miso_io_num     = SPI_MISO,
        .sclk_io_num     = SPI_CLOCK,
        .quadwp_io_num   = GPIO_NUM_NC,
        .quadhd_io_num   = GPIO_NUM_NC,
        .data4_io_num    = GPIO_NUM_NC,
        .data5_io_num    = GPIO_NUM_NC,
        .data6_io_num    = GPIO_NUM_NC,
        .data7_io_num    = GPIO_NUM_NC,
        .max_transfer_sz = SPI_MAX_TRANSFER_SIZE,
        .flags = SPICOMMON_BUSFLAG_SCLK | SPICOMMON_BUSFLAG_MISO | SPICOMMON_BUSFLAG_MOSI | SPICOMMON_BUSFLAG_MASTER,
        .intr_flags = ESP_INTR_FLAG_LOWMED | ESP_INTR_FLAG_IRAM,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_BUS, &bus, SPI_DMA_CH_AUTO));

    // LCD initialization
    ESP_LOGI(TAG, "Insstall LCD panel IO");

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num       = LCD_CS,
        .dc_gpio_num       = LCD_DC,
        .spi_mode          = 0,
        .pclk_hz           = LCD_CLOCK_HZ,
        .trans_queue_depth = LCD_SPI_QUEUE_LEN,
        .lcd_cmd_bits      = LCD_COMMAND_BITS,
        .lcd_param_bits    = LCD_PARAMETER_BITS,
        .cs_ena_pretrans   = 0,
        .cs_ena_posttrans  = 0,
        .flags = { .dc_low_on_data = 0, .octal_mode = 0, .sio_mode = 0, .lsb_first = 0, .cs_high_active = 0 },
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(LCD_SPI_BUS, &io_config, &lcd_io));

    ESP_LOGI(TAG, "Install LCD driver");

    const esp_lcd_panel_dev_config_t lcd_config = {
        .reset_gpio_num = LCD_RESET,
        .rgb_ele_order  = LCD_COLOR_MODE,
        .bits_per_pixel = LCD_BITS_PER_PIXEL,
        .flags          = { .reset_active_high = 0 },
        .vendor_config  = NULL,
    };

    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9488(lcd_io, &lcd_config, LV_BUFFER_SIZE, &lcd_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(lcd_handle, 0, 0));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_handle, true));

    return ret;
}

// static esp_err_t app_touch_init(void)
// {
//     /* Initilize I2C */
//     i2c_master_bus_handle_t       i2c_handle = NULL;
//     const i2c_master_bus_config_t i2c_config = {
//         .i2c_port   = EXAMPLE_TOUCH_I2C_NUM,
//         .sda_io_num = EXAMPLE_TOUCH_I2C_SDA,
//         .scl_io_num = EXAMPLE_TOUCH_I2C_SCL,
//         .clk_source = I2C_CLK_SRC_DEFAULT,
//     };
//     ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_config, &i2c_handle), TAG, "");

//     /* Initialize touch HW */
//     const esp_lcd_touch_config_t tp_cfg = {
//       .x_max = LCD_HORIZONTAL_PIXELS,
//       .y_max = LCD_VERTICAL_PIXELS_,
//       .rst_gpio_num = GPIO_NUM_NC, // Shared with LCD reset
//       .int_gpio_num = EXAMPLE_TOUCH_GPIO_INT,
//       .levels =
//           {
//               .reset = 0,
//               .interrupt = 0,
//           },
//       .flags =
//           {
//               .swap_xy = 0,
//               .mirror_x = 1,
//               .mirror_y = 0,
//           },
//   };
//     esp_lcd_panel_io_handle_t     tp_io_handle = NULL;
//     esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_TT21100_CONFIG();
//     tp_io_config.scl_speed_hz                  = EXAMPLE_TOUCH_I2C_CLK_HZ;
//     ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_handle, &tp_io_config, &tp_io_handle), TAG, "");
//     return esp_lcd_touch_new_i2c_tt21100(tp_io_handle, &tp_cfg, &touch_handle);
// }

// static esp_err_t app_lvgl_init(void)
// {
//     /* Initialize LVGL */
//     const lvgl_port_cfg_t lvgl_cfg = {
//         .task_priority     = 4,    /* LVGL task priority */
//         .task_stack        = 4096, /* LVGL task stack size */
//         .task_affinity     = -1,   /* LVGL task pinned to core (-1 is no affinity) */
//         .task_max_sleep_ms = 500,  /* Maximum sleep in LVGL task */
//         .timer_period_ms   = 5     /* LVGL timer tick period in ms */
//     };
//     ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

//     /* Add LCD screen */
//     ESP_LOGD(TAG, "Add LCD screen");
//     const lvgl_port_display_cfg_t disp_cfg = {
//       .io_handle = lcd_io,
//       .panel_handle = lcd_panel,
//       .buffer_size = LCD_HORIZONTAL_PIXELS * EXAMPLE_LCD_DRAW_BUFF_HEIGHT,
//       .double_buffer = EXAMPLE_LCD_DRAW_BUFF_DOUBLE,
//       .hres = LCD_HORIZONTAL_PIXELS,
//       .vres = LCD_VERTICAL_PIXELS,
//       .monochrome = false,
//       .color_format = LV_COLOR_FORMAT_RGB565,
//       .rotation =
//           {
//               .swap_xy = false,
//               .mirror_x = true,
//               .mirror_y = true,
//           },
//       .flags = {
//           .buff_dma = true,
//           .swap_bytes = true,
//       }};
//     lvgl_disp = lvgl_port_add_disp(&disp_cfg);

//     /* Add touch input (for selected screen) */
//     const lvgl_port_touch_cfg_t touch_cfg = {
//         .disp   = lvgl_disp,
//         .handle = touch_handle,
//     };
//     lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

//     return ESP_OK;
// }

// static void _app_button_cb(lv_event_t *e)
// {
//     lv_disp_rotation_t rotation = lv_disp_get_rotation(lvgl_disp);
//     rotation++;
//     if (rotation > LV_DISPLAY_ROTATION_270) {
//         rotation = LV_DISPLAY_ROTATION_0;
//     }

//     /* LCD HW rotation */
//     lv_disp_set_rotation(lvgl_disp, rotation);
// }

// static void app_main_display(void)
// {
//     lv_obj_t *scr = lv_scr_act();

//     /* Task lock */
//     lvgl_port_lock(0);

//     /* Your LVGL objects code here .... */

//     /* Create image */
//     lv_obj_t *img_logo = lv_img_create(scr);
//     lv_img_set_src(img_logo, &esp_logo);
//     lv_obj_align(img_logo, LV_ALIGN_TOP_MID, 0, 20);

//     /* Label */
//     lv_obj_t *label = lv_label_create(scr);
//     lv_obj_set_width(label, LCD_HORIZONTAL_PIXELS);
//     lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
//     lv_label_set_text(label, LV_SYMBOL_BELL " Hello world Espressif and LVGL " LV_SYMBOL_BELL "\n " LV_SYMBOL_WARNING
//                                             " For simplier initialization, use BSP " LV_SYMBOL_WARNING);
//     lv_obj_align(label, LV_ALIGN_CENTER, 0, 20);

//     /* Button */
//     lv_obj_t *btn = lv_btn_create(scr);
//     label         = lv_label_create(btn);
//     lv_label_set_text_static(label, "Rotate screen");
//     lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -30);
//     lv_obj_add_event_cb(btn, _app_button_cb, LV_EVENT_CLICKED, NULL);

//     /* Task unlock */
//     lvgl_port_unlock();
// }

void app_main(void)
{
    /* LCD HW initialization */
    ESP_ERROR_CHECK(app_lcd_init());

    /* Touch initialization */
    // ESP_ERROR_CHECK(app_touch_init());

    /* LVGL initialization */
    // ESP_ERROR_CHECK(app_lvgl_init());

    /* Show LVGL objects */
    // app_main_display();
}
