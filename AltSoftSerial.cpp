/* An Alternative Software Serial Library
 * http://www.pjrc.com/teensy/td_libs_AltSoftSerial.html
 * Copyright (c) 2014 PJRC.COM, LLC, Paul Stoffregen, paul@pjrc.com
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// Revisions are now tracked on GitHub
// https://github.com/PaulStoffregen/AltSoftSerial
//
// Version 1.2: Support Teensy 3.x
//
// Version 1.1: Improve performance in receiver code
//
// Version 1.0: Initial Release


#include "AltSoftSerial.h"
#include "config/AltSoftSerial_Boards.h"
#include "config/AltSoftSerial_Timers.h"
#include "kinetis.h"
#include "flx/string/snprintf.hpp"
#include "flx/string/join.hpp"
#include "TeensyTimerTool.h"
#include <bitset>
#include "etl/queue_spsc_atomic.h"
#include "etl/vector.h"
#include "etl/circular_buffer.h"
/****************************************/
/**          Initialization            **/
/****************************************/

static uint16_t ticks_per_bit=0;
bool AltSoftSerial::timing_error=false;

static uint8_t rx_state;
static uint8_t rx_byte;
static uint8_t rx_bit = 0;
static uint16_t rx_target;
static uint16_t rx_stop_ticks=0;
static volatile uint8_t rx_buffer_head;
static volatile uint8_t rx_buffer_tail;
#define RX_BUFFER_SIZE 80
static volatile uint8_t rx_buffer[RX_BUFFER_SIZE];

static volatile uint16_t tx_state=0U;
volatile uint16_t tx_byte;
static uint8_t tx_bit;
volatile uint16_t tx_buffer_head = 0U;
volatile uint16_t tx_buffer_tail = 0U;
#define TX_BUFFER_SIZE 68
static volatile uint8_t tx_buffer[TX_BUFFER_SIZE];


#ifndef INPUT_PULLUP
#define INPUT_PULLUP INPUT
#endif

#define MAX_COUNTS_PER_BIT  6241  // 65536 / 10.5

// volatile auto bit_index = 0;
volatile uint16_t receiving_bit_value = 0;
volatile auto incoming_byte = uint8_t{};

auto rx_queue = etl::queue_spsc_atomic<uint8_t, RX_BUFFER_SIZE, etl::memory_model::MEMORY_MODEL_SMALL>{};
// auto tx_queue = etl::circular_buffer<uint16_t, TX_BUFFER_SIZE>{};
static volatile uint16_t tx_queue[TX_BUFFER_SIZE];
// volatile uint8_t rx_bit_queue[8];
volatile uint8_t rx_bit_queue_head = 0U;

//auto rx_bit_queue = etl::queue_spsc_atomic<uint8_t, 8, etl::memory_model::MEMORY_MODEL_SMALL>{};

auto isr_timings = etl::queue_spsc_atomic<unsigned, 8, etl::memory_model::MEMORY_MODEL_SMALL>{};
auto tx_isr_timings = etl::queue_spsc_atomic<unsigned, 16, etl::memory_model::MEMORY_MODEL_SMALL>{};
auto rx_bit_queue = etl::queue_spsc_atomic<uint8_t, RX_BUFFER_SIZE, etl::memory_model::MEMORY_MODEL_SMALL>{};

auto tx_bit_flip_timings = etl::queue_spsc_atomic<std::pair<uint8_t, unsigned>, 254, etl::memory_model::MEMORY_MODEL_SMALL>{};


auto write_buffer = etl::queue_spsc_atomic<uint8_t, 1200, etl::memory_model::MEMORY_MODEL_MEDIUM>{};

volatile auto start_bit_falling_edge_clock_count = uint16_t{};
auto isr_time = 0U;

auto tx_send_timer = TeensyTimerTool::PeriodicTimer{TeensyTimerTool::FTM0};
auto end_byte_timer = TeensyTimerTool::OneShotTimer{TeensyTimerTool::FTM0};
volatile uint16_t byte_start_clock_count =0;
enum class SerialRxState {Waiting, Reading};
volatile SerialRxState serial_rx_state = SerialRxState::Waiting;

enum class SerialTxState {Waiting, Writing};
volatile SerialTxState serial_tx_state = SerialTxState::Waiting;
// void push_bit(uint8_t bit_value){
//     rx_bit_queue[rx_bit_queue_head++] = bit_value;
// }

// void push_bits(uint8_t bit_value, uint16_t num_bits){
//     for (auto i = 0U; i < num_bits; ++i){
//         push_bit(bit_value);
//     }
// }


// uint8_t pack_byte(const uint8_t* q){
//     auto byte_value = uint8_t{};
//     for (int i = 0; i < 8; ++i){
//         auto bit_value = q[i];
//         Serial1.println(flx::snprintf("read %u, %u", static_cast<unsigned>(bit_value), static_cast<unsigned>(i)).c_str());
//         byte_value |= (bit_value << i);
//     }
//     return byte_value;
// }

void write_available(){
    const auto amount_to_write = std::min(Serial1.availableForWrite(), static_cast<int>(write_buffer.size()));
    for (auto i = 0; i < amount_to_write; ++i){
        auto b = uint8_t{};
        write_buffer.pop(b);
        Serial1.write(b);
    }
}

template <size_t Size>
void add_string_to_queue(const etl::string<Size>& s){
    if (write_buffer.available() > s.size()){
        std::copy(s.begin(), s.end(), etl::push_inserter(write_buffer));
        write_buffer.push('\0');
    }
}

void process_byte(){
    rx_queue.push(uint8_t{rx_byte});
    rx_bit_queue_head = 0U;
    rx_byte = 0U;
}

void process_rx_start(uint16_t clock_start){
    end_byte_timer.trigger(std::chrono::microseconds{90});
    start_bit_falling_edge_clock_count = clock_start;
    receiving_bit_value = 0U;
    rx_bit_queue_head = 0U;
    rx_byte = 0U;
    serial_rx_state = SerialRxState::Reading;
}


// constexpr bool have_added_all_bits(uint16_t isr_start_clock_count, uint16_t read_byte_start_count, uint16_t num_bits_read, uint16_t clock_ticks_per_bit){
//     return isr_start_clock_count < read_byte_start_count + num_bits_read * clock_ticks_per_bit;
// }

constexpr uint16_t bit_index_from_clock_value(uint16_t isr_start_clock_count, uint16_t read_byte_start_count, uint16_t clock_ticks_per_bit){
    if (isr_start_clock_count < read_byte_start_count){
        return (isr_start_clock_count + (std::numeric_limits<uint16_t>::max() - read_byte_start_count)) / clock_ticks_per_bit;
    }

    return (isr_start_clock_count - read_byte_start_count) / clock_ticks_per_bit;
}

constexpr uint16_t num_bits_to_add(
    uint16_t isr_start_clock_count, uint16_t read_byte_start_count,
    uint16_t clock_ticks_per_bit, uint16_t num_bits_read){
    const auto current_bit_index =
        bit_index_from_clock_value(isr_start_clock_count, read_byte_start_count, clock_ticks_per_bit);
    return std::min(8 - num_bits_read, current_bit_index - num_bits_read);
}


constexpr uint16_t push_bits(uint16_t bit_value, uint16_t index, uint16_t num_bits){
    return static_cast<uint16_t>(((bit_value * uint16_t{0x00FF}) >> (uint16_t{8U} - num_bits)) << index);
}

constexpr std::pair<uint16_t, uint16_t> compute_new_byte_or_mask(
    uint16_t isr_start_clock_count,
    uint16_t read_byte_start_count,
    uint16_t clock_ticks_per_bit,
    uint16_t bit_index,
    uint16_t bit_value){
    const auto num_bits =
        num_bits_to_add(isr_start_clock_count, read_byte_start_count, clock_ticks_per_bit, bit_index);
    return {num_bits, push_bits(bit_value, bit_index, num_bits)};
}
void report_rx_timings(){
    if (isr_timings.empty()){
        return;
    }
    auto timings = etl::vector<unsigned, 8>{};
    while (!isr_timings.empty()){
        auto v = unsigned{};
        isr_timings.pop(v);
        float     ns = v * 1E9f / F_CPU;
        timings.push_back(static_cast<unsigned>(std::round(ns)));
    }

    add_string_to_queue(flx::snprintf("RX: [%s] ns\n", flx::join<64>(timings, ", ").c_str()));

    timings.clear();
}

void report_bit_flip_timings(){
    if (serial_tx_state == SerialTxState::Writing || tx_bit_flip_timings.empty()){
        return;
    }

    add_string_to_queue(flx::snprintf("TX Timings %u:\n", tx_bit_flip_timings.size()));
    auto previous_time = 0U;
    while (!tx_bit_flip_timings.empty()){
        auto timing = std::pair<uint8_t, unsigned>{};
        tx_bit_flip_timings.pop(timing);
        add_string_to_queue(
            flx::snprintf<20>("[%u, %u],\n", static_cast<unsigned>(timing.first), timing.second - previous_time));
        previous_time = timing.second;
    }

}

auto sent_bytes = etl::queue_spsc_atomic<uint16_t, 8, etl::memory_model::MEMORY_MODEL_SMALL>{};

void report_tx_timings(){
    report_bit_flip_timings();

    if (tx_isr_timings.empty()){
        return;
    }

    // add_string_to_queue<10>("Bytes:\n");
    // while (!sent_bytes.empty()){
    //     auto v = uint16_t{};
    //     sent_bytes.pop(v);
    //     add_string_to_queue(flx::snprintf("%u, ", v));
    // }
    // add_string_to_queue<2>("\n");

    auto timings = etl::vector<unsigned, 8>{};
    while (!tx_isr_timings.empty()){
        auto v = unsigned{};
        tx_isr_timings.pop(v);
        float     ns = v * 1E9f / F_CPU;
        timings.push_back(static_cast<unsigned>(std::round(ns)));
    }
    add_string_to_queue(flx::snprintf("TX: [%s] ns\n", flx::join<64>(timings, ", ").c_str()));
}

void do_rx_read(uint16_t isr_start_clock_count, uint16_t read_byte_start_count, uint16_t bit_value){
    constexpr auto clock_ticks_per_bit = F_CPU / 115200U;
    const auto byte_update = compute_new_byte_or_mask(isr_start_clock_count, read_byte_start_count,
        clock_ticks_per_bit, rx_bit_queue_head, bit_value);
    rx_bit_queue_head += byte_update.first;
    rx_byte |= byte_update.second;
    rx_bit_queue.push(byte_update.second);
    receiving_bit_value ^= uint16_t{1U};

    if (rx_bit_queue_head < 8U){
        return;
    }

    process_byte();
    // We caught the falling edge on the start bit
    if (rx_bit_queue_head < bit_index_from_clock_value(isr_start_clock_count, read_byte_start_count, clock_ticks_per_bit)){
        process_rx_start(isr_start_clock_count);
    }
    // report_timings();
}


void pit_isr(void)
{
    // const auto clock_count = ARM_DWT_CYCCNT;
    rx_byte |= push_bits(receiving_bit_value, rx_bit_queue_head, 8U - rx_bit_queue_head);
    process_byte();
    serial_rx_state = SerialRxState::Waiting;

    // isr_timings.push(ARM_DWT_CYCCNT - clock_count);
    // report_timings();
    // Serial1.println(flx::snprintf("interrupts: %u", static_cast<unsigned>(clock_count3 - clock_count2)).c_str());
    // for (auto index = bit_index; index < 10; ++bit_index){
    //     incoming_byte[index] = receiving_bit_value;
    // }

    // PIT has a shared IRQ for all channels on K20; service channel 0
              // one-shot
}


void receive_isr(){
    const auto clock_count = ARM_DWT_CYCCNT;
    switch (serial_rx_state){
        case SerialRxState::Waiting:
            process_rx_start(clock_count);
            isr_timings.push(ARM_DWT_CYCCNT - clock_count);
            return;
        case SerialRxState::Reading:
            do_rx_read(clock_count, start_bit_falling_edge_clock_count, receiving_bit_value);
            isr_timings.push(ARM_DWT_CYCCNT - clock_count);
            return;
    }
}

// void on_start_byte(){
//     const auto clock_count2 = ARM_DWT_CYCCNT;
//     process_rx_start(GET_TIMER_COUNT())
//    	detachInterrupt(digitalPinToInterrupt(INPUT_CAPTURE_PIN));
//     attachInterrupt(digitalPinToInterrupt(INPUT_CAPTURE_PIN), receive_isr, CHANGE);
//     isr_timings.push(ARM_DWT_CYCCNT - clock_count2);
// }

auto clock_start = 0U;
void altss_compare_a_interrupt();
void AltSoftSerial::init(uint32_t cycles_per_bit)
{
    ARM_DEMCR    |= ARM_DEMCR_TRCENA;         // enable debug/trace
        ARM_DWT_CTRL |= ARM_DWT_CTRL_CYCCNTENA;   // enable cycle counter

        // end_byte_timer.begin(pit_isr);
        // tx_send_timer.begin(altss_compare_a_interrupt, std::chrono::duration<float, std::nano>{8681.555555555555556}, false);
        // Serial1.println(flx::snprintf("Cycles %u", cycles_per_bit).c_str());
    // #if defined(ALTSS_USE_FTM1)
    //   // Enable FTM1 (timer) + PIT (timeout)
    //   CONFIG_TIMER_NOPRESCALE();
    //   // PIT_ENABLE();


    //   // Reset counter and set compare mode on TX channel
    //   SET_TIMER_COUNT(0);
    //   CONFIG_COMPARE_A_MODE();
    // #endif

	//Serial.printf("cycles_per_bit = %d\n", cycles_per_bit);
// 	if (cycles_per_bit < MAX_COUNTS_PER_BIT) {
// 		CONFIG_TIMER_NOPRESCALE();
// 	} else {
// 		cycles_per_bit /= 8;
// 		//Serial.printf("cycles_per_bit/8 = %d\n", cycles_per_bit);
// 		if (cycles_per_bit < MAX_COUNTS_PER_BIT) {
// 			CONFIG_TIMER_PRESCALE_8();
// 		} else {
// #if defined(CONFIG_TIMER_PRESCALE_256)
// 			cycles_per_bit /= 32;
// 			//Serial.printf("cycles_per_bit/256 = %d\n", cycles_per_bit);
// 			if (cycles_per_bit < MAX_COUNTS_PER_BIT) {
// 				CONFIG_TIMER_PRESCALE_256();
// 			} else {
// 				return; // baud rate too low for AltSoftSerial
// 			}
// #elif defined(CONFIG_TIMER_PRESCALE_128)
// 			cycles_per_bit /= 16;
// 			//Serial.printf("cycles_per_bit/128 = %d\n", cycles_per_bit);
// 			if (cycles_per_bit < MAX_COUNTS_PER_BIT) {
// 				CONFIG_TIMER_PRESCALE_128();
// 			} else {
// 				return; // baud rate too low for AltSoftSerial
// 			}
// #else
// 			return; // baud rate too low for AltSoftSerial
// #endif
// 		}
// 	}
   CONFIG_TIMER_NOPRESCALE();
	// FTM0_SC =  FTM_SC_CLKS(1) | FTM_SC_PS(0);
	// FTM0_C0SC = 0x48;
	// FTM0_CONF=0xC0; //set up BDM in 11
 //    FTM0_MODE|=0x0B; //enable write the FTM CnV register
 //    FTM0_CNTIN=0x00;
 //    FTM0_MOD=1000;
 //    FTM0_SC=0x48; //enable FTM0
	ticks_per_bit = cycles_per_bit;
	rx_stop_ticks = cycles_per_bit * 37 / 4;
	pinMode(INPUT_CAPTURE_PIN, INPUT_PULLUP);
	attachInterrupt(digitalPinToInterrupt(INPUT_CAPTURE_PIN), receive_isr, CHANGE);
	pinMode(OUTPUT_COMPARE_A_PIN, OUTPUT);
    digitalWrite(OUTPUT_COMPARE_A_PIN, HIGH);
	rx_state = 0;
	rx_buffer_head = 0;
	rx_buffer_tail = 0;
	tx_state = 0;
	tx_buffer_head = 0;
	tx_buffer_tail = 0;
	// ENABLE_INT_INPUT_CAPTURE();
}

void AltSoftSerial::report(){
    write_available();

    report_tx_timings();
    if (rx_queue.empty()){
        return;
    }

    auto bits = etl::vector<uint16_t, 8>{};
    while (!rx_bit_queue.empty()){
        auto v = uint8_t{};
        rx_bit_queue.pop(v);
        bits.push_back(v);
    }
    add_string_to_queue(flx::snprintf("RX bits: %s\n", flx::join<64>(bits, ", ").c_str()));

    while (!rx_queue.empty()){
        auto v = uint8_t{};
        rx_queue.pop(v);
        add_string_to_queue(flx::snprintf("%u,", static_cast<unsigned int>(v)));
        add_string_to_queue(etl::string<2>{"\n"});
        report_rx_timings();
    }
}

void AltSoftSerial::end(void)
{
    #ifndef ALTSS_USE_FTM1
	DISABLE_INT_COMPARE_B();
	DISABLE_INT_INPUT_CAPTURE();
	flushInput();
	flushOutput();
	DISABLE_INT_COMPARE_A();
	// TODO: restore timer to original settings?
	#endif
}


/****************************************/
/**           Transmission             **/
/****************************************/
void set_next_clock_firing(uint16_t clock){
    clock_start = clock + ticks_per_bit;
    FTM0_C6V = clock_start;
}

constexpr uint16_t prepare_new_payload(uint8_t byte_to_write){
    return (0xFFu<<9U) | (static_cast<uint16_t>(byte_to_write) << 1U);
}

// uint16_t start_tx_new_byte(){
//     auto new_byte = uint16_t{};
//     tx_queue.pop(new_byte);
//     return new_byte;
//     // tx_byte = new_byte;
//     // tx_state = 0U;
// }


bool is_tx_buffer_empty(){
    return tx_buffer_head == tx_buffer_tail;
}

bool is_tx_buffer_full(){
    return ((tx_buffer_head + 1) % TX_BUFFER_SIZE) == tx_buffer_tail;
}

void push_to_tx_buffer(uint16_t value){
    const auto head = tx_buffer_head;
    tx_queue[head] = value;
    tx_buffer_head = (head + 1) % TX_BUFFER_SIZE;
}

uint16_t pop_from_tx_buffer(){
    auto tail = tx_buffer_tail;
    auto value = tx_queue[tail++];
    tx_buffer_tail = (tail < TX_BUFFER_SIZE) ? tail : 0U;
    return value;
}

void AltSoftSerial::writeByte(uint8_t b)
{
    while (is_tx_buffer_full()){}
    push_to_tx_buffer(prepare_new_payload(b));

    if (serial_tx_state == SerialTxState::Writing){
        return;
    }

    serial_tx_state = SerialTxState::Writing;
    // start_tx_new_byte();
    FTM0_C6V = GET_TIMER_COUNT() + ticks_per_bit;
    ENABLE_INT_COMPARE_A();
    CONFIG_MATCH_CLEAR();
    // set_next_clock_firing(GET_TIMER_COUNT());
}

constexpr uint16_t bit_to_write(uint16_t bit_address, uint16_t byte_to_write){
    return (byte_to_write >> bit_address) & 1;
}

void write_next_bit(uint16_t byte_value, uint16_t bit_index, uint16_t clock_start_time){

    // if (!tx_bit_flip_timings.full()){tx_bit_flip_timings.push({bit, ARM_DWT_CYCCNT});}
     //digitalWriteFast(OUTPUT_COMPARE_A_PIN,  bit_to_write(bit_index, byte_value););
    // FTM0_C6V = clock_start_time;
    // set_next_clock_firing(clock_start);
	// tx_state++;
}


void stop_writing(){
    serial_tx_state = SerialTxState::Waiting;
     // if (!tx_bit_flip_timings.full()){tx_bit_flip_timings.push({1, ARM_DWT_CYCCNT});}
    digitalWriteFast(OUTPUT_COMPARE_A_PIN, HIGH);
    // CONFIG_MATCH_NORMAL();
    DISABLE_INT_COMPARE_A();
}

void altss_compare_a_interrupt()
{
    static auto test_tx_state = uint16_t{0U};
    static auto byte_to_write = uint16_t{0xFFFF};
    static auto clock_start_time = GET_TIMER_COUNT();

    const auto clock_count2 = ARM_DWT_CYCCNT;
    const auto bit = bit_to_write(test_tx_state, byte_to_write);
    //if (!tx_bit_flip_timings.full()){tx_bit_flip_timings.push({bit, ARM_DWT_CYCCNT});}
    digitalWriteFast(OUTPUT_COMPARE_A_PIN,  bit);
    test_tx_state = (test_tx_state == 9U) ? 0U : test_tx_state + 1;
    clock_start_time += ticks_per_bit;
    FTM0_C6V = clock_start_time;

    if (test_tx_state > 0U){
        tx_isr_timings.push(ARM_DWT_CYCCNT - clock_count2);
        return;
	}

   	if (is_tx_buffer_empty()){
        byte_to_write = 0xFFFF;
        stop_writing();
        tx_isr_timings.push(ARM_DWT_CYCCNT - clock_count2);
        //add_string_to_queue<4>("\n\n");
        return;
    }

    // start_tx_new_byte();
    //if (!tx_bit_flip_timings.full()){tx_bit_flip_timings.push({0U, ARM_DWT_CYCCNT});}
    // clock_start_time = GET_TIMER_COUNT() + ticks_per_bit;
    // digitalWriteFast(OUTPUT_COMPARE_A_PIN, LOW);
    byte_to_write = pop_from_tx_buffer();
    // test_tx_state = 0U;
	// FTM0_C6V = clock_start_time;
    tx_isr_timings.push(ARM_DWT_CYCCNT - clock_count2);
    // add_string_to_queue<2>("\n");
    // altss_compare_a_interrupt();
    // clock_start_time += 16;
    // FTM0_C6V = clock_start_time;
}


// void altss_compare_a_interrupt()
// {
// 	uint8_t state, byte, bit, head, tail;
// 	uint16_t target;

// 	state = tx_state;
// 	byte = tx_byte;
// 	target = GET_COMPARE_A();
// 	if (state > 0 && state < 10){
// 	    digitalWriteFast(OUTPUT_COMPARE_A_PIN, tx_bit);
// 	}
// 	while (state < 10) {
// 	    // Serial1.println(flx::snprintf("State %d", state).c_str());
// 		target += ticks_per_bit;
// 		if (state < 9)
// 			bit = byte & 1;
// 		else
// 			bit = 1; // stopbit
// 		byte >>= 1;
// 		state++;
// 		// Serial1.print(flx::snprintf("%d,", bit).c_str());

// 		if (bit != tx_bit) {
// 			if (bit) {
// 				CONFIG_MATCH_SET();
//                 // digitalWriteFast(OUTPUT_COMPARE_A_PIN, HIGH);
//                 //Serial1.print("1");
// 			} else {
//     			CONFIG_MATCH_CLEAR();
//     			// digitalWriteFast(OUTPUT_COMPARE_A_PIN, LOW);
//                 //Serial1.print("0");
// 			}
// 			SET_COMPARE_A(target);
// 			tx_bit = bit;
// 			tx_byte = byte;
// 			tx_state = state;
// 			// Serial1.println(flx::snprintf("Done, state %d", tx_state).c_str());

// 			// TODO: how to detect timing_error?
// 			return;
// 		}
// 	}
// 	head = tx_buffer_head;
// 	tail = tx_buffer_tail;
// 	// Serial1.println(flx::snprintf("Head, tail %d,%d", static_cast<int>(head), static_cast<int>(tail)).c_str());
// 	if (head == tail) {
// 		if (state == 10) {
// 			// Wait for final stop bit to finish
// 			tx_state = 11;
// 			// Serial1.print(flx::snprintf("%d,", 1).c_str());
// 			digitalWriteFast(OUTPUT_COMPARE_A_PIN, HIGH);
// 			SET_COMPARE_A(target + ticks_per_bit);
// 		} else {
// 			tx_state = 0;
// 			digitalWriteFast(OUTPUT_COMPARE_A_PIN, HIGH);
// 			CONFIG_MATCH_NORMAL();
// 			// Serial1.println("Disable compare a");
// 			DISABLE_INT_COMPARE_A();
// 		}
// 	} else {
// 		if (++tail >= TX_BUFFER_SIZE) tail = 0;
// 		tx_buffer_tail = tail;
// 		tx_byte = tx_buffer[tail];
// 		tx_bit = 0;
//         // Serial1.print(flx::snprintf("%d,", 0).c_str());
//         digitalWriteFast(OUTPUT_COMPARE_A_PIN, LOW);
// 		CONFIG_MATCH_CLEAR();
// 		if (state == 10)
// 			SET_COMPARE_A(target + ticks_per_bit);
// 		else
// 			SET_COMPARE_A(GET_TIMER_COUNT() + 16);
// 		tx_state = 1;
// 		// TODO: how to detect timing_error?
// 	}
// }

void AltSoftSerial::flushOutput(void)
{
	while (tx_state) /* wait */ ;
}


/****************************************/
/**            Reception               **/
/****************************************/

// void altss_capture_interrupt()
// {
// 	uint8_t state, bit, head;
// 	uint16_t capture, target;
// 	uint16_t offset, offset_overflow;

// 	Serial1.println(flx::snprintf("Caught edge %d", static_cast<int>(rx_bit)).c_str());
// 	capture = GET_INPUT_CAPTURE();
// 	bit = rx_bit;
// 	if (bit) {
// 		CONFIG_CAPTURE_FALLING_EDGE();
// 		rx_bit = 0;
// 	} else {
// 		CONFIG_CAPTURE_RISING_EDGE();
// 		rx_bit = 0x80;
// 	}
// 	state = rx_state;
// 	if (state == 0) {
// 		if (!bit) {
// 		#if defined(ALTSS_USE_FTM1)
//         // Arm one-shot timeout on PIT0. Choose the same tick base as FTM1:
//         // If FTM1 runs at sysclk/prescale, compute PIT ticks accordingly.
//         // Easiest: run PIT at bus clock and scale rx_stop_ticks to PIT LDVAL.
//         // For identical tick bases, assume FTM1 uses system clock == PIT clock.
//         PIT0_CLEAR_FLAG();
//         PIT0_SET_TICKS(rx_stop_ticks);  // if clocks match 1:1; otherwise scale here
//         PIT0_START();
//         #else
//         uint16_t end = capture + rx_stop_ticks;
//         SET_COMPARE_B(end);
//         ENABLE_INT_COMPARE_B();
//         #endif
// 			rx_target = capture + ticks_per_bit + ticks_per_bit/2;
// 			rx_state = 1;
// 		}
// 	} else {
// 		target = rx_target;
// 		offset_overflow = 65535 - ticks_per_bit;
// 		while (1) {
// 			offset = capture - target;
// 			if (offset > offset_overflow) break;
// 			rx_byte = (rx_byte >> 1) | rx_bit;
// 			target += ticks_per_bit;
// 			state++;
// 			if (state >= 9) {
// #if defined(ALTSS_USE_FTM1)
// 			    PIT0_STOP();
// #else
// 				DISABLE_INT_COMPARE_B();
// #endif
// 				head = rx_buffer_head + 1;
// 				if (head >= RX_BUFFER_SIZE) head = 0;
// 				if (head != rx_buffer_tail) {
// 					rx_buffer[head] = rx_byte;
// 					rx_buffer_head = head;
// 				}
// 				CONFIG_CAPTURE_FALLING_EDGE();
// 				rx_bit = 0;
// 				rx_state = 0;
// 				return;
// 			}
// 		}
// 		rx_target = target;
// 		rx_state = state;
// 	}
// 	//if (GET_TIMER_COUNT() - capture > ticks_per_bit) AltSoftSerial::timing_error = true;
// }

// void altss_compare_b_interrupt()
// {
// 	uint8_t head, state, bit;

// 	DISABLE_INT_COMPARE_B();
// 	CONFIG_CAPTURE_FALLING_EDGE();
// 	state = rx_state;
// 	bit = rx_bit ^ 0x80;
// 	while (state < 9) {
// 		rx_byte = (rx_byte >> 1) | bit;
// 		state++;
// 	}
// 	head = rx_buffer_head + 1;
// 	if (head >= RX_BUFFER_SIZE) head = 0;
// 	if (head != rx_buffer_tail) {
// 		rx_buffer[head] = rx_byte;
// 		rx_buffer_head = head;
// 	}
// 	rx_state = 0;
// 	CONFIG_CAPTURE_FALLING_EDGE();
// 	rx_bit = 0;
// }


int AltSoftSerial::read(void)
{
	uint8_t head, tail, out;

	head = rx_buffer_head;
	tail = rx_buffer_tail;
	if (head == tail) return -1;
	if (++tail >= RX_BUFFER_SIZE) tail = 0;
	out = rx_buffer[tail];
	rx_buffer_tail = tail;
	return out;
}

int AltSoftSerial::peek(void)
{
	uint8_t head, tail;

	head = rx_buffer_head;
	tail = rx_buffer_tail;
	if (head == tail) return -1;
	if (++tail >= RX_BUFFER_SIZE) tail = 0;
	return rx_buffer[tail];
}

int AltSoftSerial::available(void)
{
	uint8_t head, tail;

	head = rx_buffer_head;
	tail = rx_buffer_tail;
	if (head >= tail) return head - tail;
	return RX_BUFFER_SIZE + head - tail;
}

int AltSoftSerial::availableForWrite(void)
{
	uint8_t head, tail;
	head = tx_buffer_head;
	tail = tx_buffer_tail;

	if (tail > head) return tail - head;
	return TX_BUFFER_SIZE + tail - head;
};

void AltSoftSerial::flushInput(void)
{
	rx_buffer_head = rx_buffer_tail;
}

void ftm0_isr(void)
{
	uint32_t flags = FTM0_STATUS;
	FTM0_STATUS = 0;

	// if (flags & (1<<0) && (FTM0_C0SC & 0x40)) altss_compare_b_interrupt();
	// if (flags & (1<<5)) altss_capture_interrupt();
	if (flags & (1<<6) && (FTM0_C6SC & 0x40)) {
	altss_compare_a_interrupt();
	}
}

// #if defined(ALTSS_USE_FTM1)
// extern "C" void ftm1_isr(void)
// {
//   uint32_t flags = FTM1_STATUS;
//   FTM1_STATUS = 0;

//   // RX capture on CH0?
//   if ((flags & (1u << 0)) && (FTM1_C0SC & 0x40)) {
//     Serial1.println(flx::snprintf("ISR Flags: %u",flags).c_str());
//     altss_capture_interrupt();
//   }

//   // TX compare on CH1?
//   if ((flags & (1u << 1)) && (FTM1_C1SC & 0x40)) {

//     altss_compare_a_interrupt();
//   }
// }
// #endif
