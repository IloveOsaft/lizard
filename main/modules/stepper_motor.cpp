#include "stepper_motor.h"
#include "../utils/uart.h"
#include <driver/ledc.h>
#include <driver/pcnt.h>
#include <memory>

StepperMotor::StepperMotor(const std::string name, const gpio_num_t step_pin, const gpio_num_t dir_pin)
    : Module(output, name), step_pin(step_pin), dir_pin(dir_pin) {
    gpio_reset_pin(step_pin);
    gpio_reset_pin(dir_pin);

    pcnt_config_t pcnt_config = {
        .pulse_gpio_num = step_pin,
        .ctrl_gpio_num = dir_pin,
        .lctrl_mode = PCNT_MODE_REVERSE,
        .hctrl_mode = PCNT_MODE_KEEP,
        .pos_mode = PCNT_COUNT_INC,
        .neg_mode = PCNT_COUNT_DIS,
        .counter_h_lim = 32767,
        .counter_l_lim = -32768,
        .unit = PCNT_UNIT_0,
        .channel = PCNT_CHANNEL_0,
    };
    pcnt_unit_config(&pcnt_config);
    pcnt_counter_pause(PCNT_UNIT_0);
    pcnt_counter_clear(PCNT_UNIT_0);
    pcnt_counter_resume(PCNT_UNIT_0);

    ledc_timer_config_t timer_config = {
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_2_BIT,
        .timer_num = LEDC_TIMER_3,
        .freq_hz = 1000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_channel_config_t channel_config = {
        .gpio_num = step_pin,
        .speed_mode = LEDC_HIGH_SPEED_MODE,
        .channel = LEDC_CHANNEL_5,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_3,
        .duty = 1,
        .hpoint = 0,
    };
    ledc_timer_config(&timer_config);
    ledc_channel_config(&channel_config);
    ledc_timer_pause(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_3);
    gpio_set_direction(step_pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_direction(dir_pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_matrix_out(step_pin, LEDC_HS_SIG_OUT0_IDX + LEDC_CHANNEL_5, 0, 0);
}

void StepperMotor::step() {
    int16_t count;
    pcnt_get_counter_value(PCNT_UNIT_0, &count);
    // echo("count = %d", count);
    Module::step();
}

void StepperMotor::call(const std::string method_name, const std::vector<ConstExpression_ptr> arguments) {
    if (method_name == "forward") {
        Module::expect(arguments, 1, numbery);
        double frequency = arguments[0]->evaluate_number();
        echo("Forward with %.3f hz", frequency);
        gpio_set_level(this->dir_pin, 1);
        ledc_set_freq(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_3, (uint32_t)frequency);
        ledc_timer_resume(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_3);
    } else if (method_name == "backward") {
        Module::expect(arguments, 1, numbery);
        double frequency = arguments[0]->evaluate_number();
        echo("Backward with %.3f hz", frequency);
        gpio_set_level(this->dir_pin, 0);
        ledc_set_freq(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_3, (uint32_t)frequency);
        ledc_timer_resume(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_3);
    } else if (method_name == "stop") {
        Module::expect(arguments, 0);
        echo("Stop");
        ledc_timer_pause(LEDC_HIGH_SPEED_MODE, LEDC_TIMER_3);
    } else {
        Module::call(method_name, arguments);
    }
}
