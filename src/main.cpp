#include "compilation/await_condition.h"
#include "compilation/await_routine.h"
#include "compilation/expression.h"
#include "compilation/method_call.h"
#include "compilation/routine.h"
#include "compilation/routine_call.h"
#include "compilation/rule.h"
#include "compilation/variable.h"
#include "compilation/variable_assignment.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "global.h"
#include "modules/core.h"
#include "modules/module.h"
#include "storage.h"
#include "utils/echo.h"
#include "utils/tictoc.h"
#include "utils/timing.h"
#include <chrono>
#include <memory>
#include <stdexcept>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#define BUFFER_SIZE 1024

Core_ptr core_module;

extern "C" {
#include "parser.h"
void app_main();
}

std::string identifier_to_string(const struct owl_ref ref) {
    const struct parsed_identifier identifier = parsed_identifier_get(ref);
    return std::string(identifier.identifier, identifier.length);
}

Expression_ptr compile_expression(const struct owl_ref ref);

std::vector<ConstExpression_ptr> compile_arguments(const struct owl_ref ref) {
    std::vector<ConstExpression_ptr> arguments;
    for (struct owl_ref r = ref; !r.empty; r = owl_next(r)) {
        arguments.push_back(compile_expression(r));
    }
    return arguments;
}

Expression_ptr compile_expression(const struct owl_ref ref) {
    const struct parsed_expression expression = parsed_expression_get(ref);
    switch (expression.type) {
    case PARSED_TRUE:
        return std::make_shared<BooleanExpression>(true);
    case PARSED_FALSE:
        return std::make_shared<BooleanExpression>(false);
    case PARSED_STRING: {
        const struct parsed_string string = parsed_string_get(expression.string);
        return std::make_shared<StringExpression>(std::string(string.string, string.length));
    }
    case PARSED_INTEGER:
        return std::make_shared<IntegerExpression>(parsed_integer_get(expression.integer).integer);
    case PARSED_NUMBER:
        return std::make_shared<NumberExpression>(parsed_number_get(expression.number).number);
    case PARSED_VARIABLE:
        return std::make_shared<VariableExpression>(Global::get_variable(identifier_to_string(expression.identifier)));
    case PARSED_PROPERTY:
        return std::make_shared<VariableExpression>(Global::get_module(identifier_to_string(expression.module_name))
                                                        ->get_property(identifier_to_string(expression.property_name)));
    case PARSED_PARENTHESES:
        return compile_expression(expression.expression);
    case PARSED_POWER:
        return std::make_shared<PowerExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_NEGATE:
        return std::make_shared<NegateExpression>(compile_expression(expression.operand));
    case PARSED_MULTIPLY:
        return std::make_shared<MultiplyExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_DIVIDE:
        return std::make_shared<DivideExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_ADD:
        return std::make_shared<AddExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_SUBTRACT:
        return std::make_shared<SubtractExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_SHIFT_LEFT:
        return std::make_shared<ShiftLeftExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_SHIFT_RIGHT:
        return std::make_shared<ShiftRightExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_BIT_AND:
        return std::make_shared<BitAndExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_BIT_XOR:
        return std::make_shared<BitXorExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_BIT_OR:
        return std::make_shared<BitOrExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_GREATER:
        return std::make_shared<GreaterExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_LESS:
        return std::make_shared<LessExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_GREATER_EQUAL:
        return std::make_shared<GreaterEqualExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_LESS_EQUAL:
        return std::make_shared<LessEqualExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_EQUAL:
        return std::make_shared<EqualExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_UNEQUAL:
        return std::make_shared<UnequalExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_NOT:
        return std::make_shared<NotExpression>(compile_expression(expression.operand));
    case PARSED_AND:
        return std::make_shared<AndExpression>(compile_expression(expression.left), compile_expression(expression.right));
    case PARSED_OR:
        return std::make_shared<OrExpression>(compile_expression(expression.left), compile_expression(expression.right));
    default:
        throw std::runtime_error("invalid expression");
    }
}

std::vector<Action_ptr> compile_actions(const struct owl_ref ref) {
    std::vector<Action_ptr> actions;
    for (struct owl_ref r = ref; !r.empty; r = owl_next(r)) {
        const struct parsed_action action = parsed_action_get(r);
        if (!action.noop.empty) {
        } else if (!action.method_call.empty) {
            const struct parsed_method_call method_call = parsed_method_call_get(action.method_call);
            const std::string module_name = identifier_to_string(method_call.module_name);
            const Module_ptr module = Global::get_module(module_name);
            const std::string method_name = identifier_to_string(method_call.method_name);
            const std::vector<ConstExpression_ptr> arguments = compile_arguments(method_call.argument);
            actions.push_back(std::make_shared<MethodCall>(module, method_name, arguments));
        } else if (!action.routine_call.empty) {
            const struct parsed_routine_call routine_call = parsed_routine_call_get(action.routine_call);
            const std::string routine_name = identifier_to_string(routine_call.routine_name);
            const Routine_ptr routine = Global::get_routine(routine_name);
            actions.push_back(std::make_shared<RoutineCall>(routine));
        } else if (!action.variable_assignment.empty) {
            const struct parsed_variable_assignment variable_assignment = parsed_variable_assignment_get(action.variable_assignment);
            const std::string variable_name = identifier_to_string(variable_assignment.variable_name);
            const Variable_ptr variable = Global::get_variable(variable_name);
            const ConstExpression_ptr expression = compile_expression(variable_assignment.expression);
            if (variable->type != expression->type) {
                throw std::runtime_error("type mismatch for variable assignment");
            }
            if (variable->type == identifier) {
                throw std::runtime_error("assignment of identifiers is forbidden");
            }
            actions.push_back(std::make_shared<VariableAssignment>(variable, expression));
        } else if (!action.await_condition.empty) {
            struct parsed_await_condition await_condition = parsed_await_condition_get(action.await_condition);
            const ConstExpression_ptr condition = compile_expression(await_condition.condition);
            actions.push_back(std::make_shared<AwaitCondition>(condition));
        } else if (!action.await_routine.empty) {
            struct parsed_await_routine await_routine = parsed_await_routine_get(action.await_routine);
            const std::string routine_name = identifier_to_string(await_routine.routine_name);
            const Routine_ptr routine = Global::get_routine(routine_name);
            actions.push_back(std::make_shared<AwaitRoutine>(routine));
        } else {
            throw std::runtime_error("unknown action type");
        }
    }
    return actions;
}

void process_tree(owl_tree *const tree) {
    const struct parsed_statements statements = owl_tree_get_parsed_statements(tree);
    for (struct owl_ref r = statements.statement; !r.empty; r = owl_next(r)) {
        const struct parsed_statement statement = parsed_statement_get(r);
        if (!statement.noop.empty) {
        } else if (!statement.expression.empty) {
            const ConstExpression_ptr expression = compile_expression(statement.expression);
            static char buffer[256];
            expression->print_to_buffer(buffer);
            echo(up, text, buffer);
        } else if (!statement.constructor.empty) {
            const struct parsed_constructor constructor = parsed_constructor_get(statement.constructor);
            const std::string module_name = identifier_to_string(constructor.module_name);
            if (Global::has_module(module_name)) {
                throw std::runtime_error("module \"" + module_name + "\" already exists");
            }
            const std::string module_type = identifier_to_string(constructor.module_type);
            const std::vector<ConstExpression_ptr> arguments = compile_arguments(constructor.argument);
            Global::add_module(module_name, Module::create(module_type, module_name, arguments));
        } else if (!statement.method_call.empty) {
            const struct parsed_method_call method_call = parsed_method_call_get(statement.method_call);
            const std::string module_name = identifier_to_string(method_call.module_name);
            const Module_ptr module = Global::get_module(module_name);
            const std::string method_name = identifier_to_string(method_call.method_name);
            const std::vector<ConstExpression_ptr> arguments = compile_arguments(method_call.argument);
            module->call_with_shadows(method_name, arguments);
        } else if (!statement.routine_call.empty) {
            const struct parsed_routine_call routine_call = parsed_routine_call_get(statement.routine_call);
            const std::string routine_name = identifier_to_string(routine_call.routine_name);
            const Routine_ptr routine = Global::get_routine(routine_name);
            if (routine->is_running()) {
                throw std::runtime_error("routine \"" + routine_name + "\" is already running");
            }
            routine->start();
        } else if (!statement.property_assignment.empty) {
            const struct parsed_property_assignment property_assignment = parsed_property_assignment_get(statement.property_assignment);
            const std::string module_name = identifier_to_string(property_assignment.module_name);
            const Module_ptr module = Global::get_module(module_name);
            const std::string property_name = identifier_to_string(property_assignment.property_name);
            const ConstExpression_ptr expression = compile_expression(property_assignment.expression);
            module->write_property(property_name, expression);
        } else if (!statement.variable_assignment.empty) {
            const struct parsed_variable_assignment variable_assignment = parsed_variable_assignment_get(statement.variable_assignment);
            const std::string variable_name = identifier_to_string(variable_assignment.variable_name);
            const Variable_ptr variable = Global::get_variable(variable_name);
            const ConstExpression_ptr expression = compile_expression(variable_assignment.expression);
            variable->assign(expression);
        } else if (!statement.variable_declaration.empty) {
            const struct parsed_variable_declaration variable_declaration = parsed_variable_declaration_get(statement.variable_declaration);
            const struct parsed_datatype datatype = parsed_datatype_get(variable_declaration.datatype);
            const std::string variable_name = identifier_to_string(variable_declaration.variable_name);
            switch (datatype.type) {
            case PARSED_BOOLEAN:
                Global::add_variable(variable_name, std::make_shared<BooleanVariable>());
                break;
            case PARSED_INTEGER:
                Global::add_variable(variable_name, std::make_shared<IntegerVariable>());
                break;
            case PARSED_NUMBER:
                Global::add_variable(variable_name, std::make_shared<NumberVariable>());
                break;
            case PARSED_STRING:
                Global::add_variable(variable_name, std::make_shared<StringVariable>());
                break;
            default:
                throw std::runtime_error("invalid data type for variable declaration");
            }
            if (!variable_declaration.expression.empty) {
                const ConstExpression_ptr expression = compile_expression(variable_declaration.expression);
                Global::get_variable(variable_name)->assign(expression);
            }
        } else if (!statement.routine_definition.empty) {
            const struct parsed_routine_definition routine_definition = parsed_routine_definition_get(statement.routine_definition);
            const std::string routine_name = identifier_to_string(routine_definition.routine_name);
            if (Global::has_routine(routine_name)) {
                throw std::runtime_error("routine \"" + routine_name + "\" already exists");
            }
            const struct parsed_actions actions = parsed_actions_get(routine_definition.actions);
            Global::add_routine(routine_name, std::make_shared<Routine>(compile_actions(actions.action)));
        } else if (!statement.rule_definition.empty) {
            const struct parsed_rule_definition rule_definition = parsed_rule_definition_get(statement.rule_definition);
            const struct parsed_actions actions = parsed_actions_get(rule_definition.actions);
            const Routine_ptr routine = std::make_shared<Routine>(compile_actions(actions.action));
            const ConstExpression_ptr condition = compile_expression(rule_definition.condition);
            Global::add_rule(std::make_shared<Rule>(condition, routine));
        } else {
            throw std::runtime_error("unknown statement type");
        }
    }
}

void process_lizard(const char *line) {
    const bool debug = core_module->get_property("debug")->boolean_value;
    if (debug) {
        echo(up, text, ">> %s", line);
        tic();
    }
    struct owl_tree *const tree = owl_tree_create_from_string(line);
    if (debug) {
        toc("Tree creation");
    }
    struct source_range range;
    switch (owl_tree_get_error(tree, &range)) {
    case ERROR_INVALID_FILE:
        echo(up, text, "error: invalid file");
        break;
    case ERROR_INVALID_OPTIONS:
        echo(up, text, "error: invalid options");
        break;
    case ERROR_INVALID_TOKEN:
        echo(up, text, "error: invalid token at range %zu %zu \"%s\"", range.start, range.end,
             std::string(line, range.start, range.end - range.start).c_str());
        break;
    case ERROR_UNEXPECTED_TOKEN:
        echo(up, text, "error: unexpected token at range %zu %zu \"%s\"", range.start, range.end,
             std::string(line, range.start, range.end - range.start).c_str());
        break;
    case ERROR_MORE_INPUT_NEEDED:
        echo(up, text, "error: more input needed at range %zu %zu", range.start, range.end);
        break;
    default:
        if (debug) {
            owl_tree_print(tree);
            tic();
        }
        process_tree(tree);
        if (debug) {
            toc("Tree traversal");
        }
    }
    owl_tree_destroy(tree);
}

void deactivate_uart1() {
    uart_driver_delete(UART_NUM_1);

    gpio_reset_pin(GPIO_NUM_26);
    gpio_reset_pin(GPIO_NUM_27);
    gpio_set_direction(GPIO_NUM_26, GPIO_MODE_INPUT);
    gpio_set_direction(GPIO_NUM_27, GPIO_MODE_INPUT);
    gpio_set_pull_mode(GPIO_NUM_26, GPIO_FLOATING);
    gpio_set_pull_mode(GPIO_NUM_27, GPIO_FLOATING);
}

void process_line(const char *line, const int len, const uart_port_t uart_num) {
    if (len >= 2 && line[0] == '!') {
        switch (line[1]) {
        case '+':
            Storage::append_to_startup(line + 2);
            break;
        case '-':
            Storage::remove_from_startup(line + 2);
            break;
        case '?':
            Storage::print_startup(line + 2);
            break;
        case '.':
            Storage::save_startup();
            break;
        case '!':
            process_lizard(line + 2);
            break;
        case '"':
            echo(up, text, line + 2);
            break;
        case '>':
            echo(down, raw, line + 2);
            break;
        case '~':
            deactivate_uart1();
            break;
        default:
            throw std::runtime_error("unrecognized control command");
        }
    } else {
        if (uart_num == UART_NUM_1) {
            printf("%s\n", line);
        } else {
            process_lizard(line);
        }
    }
}

void process_uart(const uart_port_t uart_num) {
    static char input[BUFFER_SIZE];

    if (!uart_is_driver_installed(uart_num)) {
        return;
    }

    while (true) {
        int pos = uart_pattern_get_pos(uart_num);
        if (pos < 0) {
            break;
        }

        int len = uart_read_bytes(uart_num, (uint8_t *)input, pos + 1, 0);
        int suffix = 0;
        if (len >= 5 && input[len - 2] == '\r' && input[len - 5] == '@') {
            suffix = 5;
        } else if (len >= 4 && input[len - 4] == '@') {
            suffix = 4;
        }
        if (suffix) {
            uint8_t checksum = 0;
            for (int i = 0; i < len - suffix; ++i) {
                checksum ^= input[i];
            }
            const std::string hex_number(&input[len - suffix + 1], 2);
            if (std::stoi(hex_number, 0, 16) != checksum) {
                throw std::runtime_error("checksum mismatch");
            }
            len -= suffix;
        } else {
            len -= 1;
        }
        input[len] = 0;

        try {
            process_line(input, len, uart_num);
        } catch (const std::runtime_error &e) {
            echo(up, text, "error while processing line from UART %d: %s", uart_num, e.what());
        }
    }
}

void app_main() {
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .use_ref_tick = false,
    };
    uart_param_config(UART_NUM_0, &uart_config);
    uart_param_config(UART_NUM_1, &uart_config);
    uart_set_pin(UART_NUM_1, GPIO_NUM_27, GPIO_NUM_26, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_driver_install(UART_NUM_0, BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_driver_install(UART_NUM_1, BUFFER_SIZE * 2, 0, 0, NULL, 0);
    uart_enable_pattern_det_baud_intr(UART_NUM_0, '\n', 1, 9, 0, 0);
    uart_enable_pattern_det_baud_intr(UART_NUM_1, '\n', 1, 9, 0, 0);
    uart_pattern_queue_reset(UART_NUM_0, 100);
    uart_pattern_queue_reset(UART_NUM_1, 100);

    printf("Ready.\n");

    try {
        Global::add_module("core", core_module = std::make_shared<Core>("core"));
    } catch (const std::runtime_error &e) {
        echo(up, text, "error while initializing core module: %s", e.what());
        exit(1);
    }

    try {
        Storage::init();
        process_lizard(Storage::startup.c_str());
    } catch (const std::runtime_error &e) {
        echo(up, text, "error while loading startup script: %s", e.what());
    }

    while (true) {
        process_uart(UART_NUM_0);
        process_uart(UART_NUM_1);

        for (auto const &[module_name, module] : Global::modules) {
            try {
                module->step();
            } catch (const std::runtime_error &e) {
                echo(up, text, "error in module \"%s\": %s", module_name.c_str(), e.what());
            }
        }

        for (auto const &rule : Global::rules) {
            try {
                if (rule->condition->evaluate_boolean() && !rule->routine->is_running()) {
                    rule->routine->start();
                }
                rule->routine->step();
            } catch (const std::runtime_error &e) {
                echo(up, text, "error in rule: %s", e.what());
            }
        }

        for (auto const &[routine_name, routine] : Global::routines) {
            try {
                routine->step();
            } catch (const std::runtime_error &e) {
                echo(up, text, "error in routine \"%s\": %s", routine_name.c_str(), e.what());
            }
        }

        delay(10);
    }
}
