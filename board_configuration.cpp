// PRO-MS6 - Bosch MS6.0 plug & play replacement, STM32F407VGT6 (U202)
// Pin map taken from the PRO-MS6 KiCad schematic. J102 = main 48-pin Molex, J101 = 26-pin aux.

#include "pch.h"
#include "board_overrides.h"
#include "defaults.h"
#include "settings.h"
#include "mpu_util.h"

void setupBoardHardwareTestOverrides();

// PE10 -> /Power/EN switches +5VA (VR, analog buffers, digital inputs, MIC4427) and
// +3.3VA (on-board wideband). The MCU itself is always powered from 12V_Perm.
static OutputPin powerEnablePin;

// PWR/PA0 state captured in preHalInit, printed once the console is up
static uint32_t bootPwrCsr;
static bool bootPa0High;

static void customBoardInitHardwareEarly() {
	powerEnablePin.initPin("Power EN", Gpio::E10);
	powerEnablePin.setValue(1);

	efiPrintf("PRO-MS6 boot: SBF=%d WUF=%d PA0=%d resetCause=%s",
		(int)((bootPwrCsr & PWR_CSR_SBF) != 0), (int)((bootPwrCsr & PWR_CSR_WUF) != 0), (int)bootPa0High,
		getMCUResetCause(getMCUResetCause()));
}

// Enter STM32 standby with PA0 rising edge as the only wakeup source. Unlike stm32_standby()
// this never returns: if WFI falls through (pending wakeup flag) it simply tries again.
[[noreturn]] static void enterStandby() {
	__disable_irq();
	SysTick->CTRL = 0;

	// Stop CAN: a frame nobody ACKs (wideband already unpowered) is retransmitted forever,
	// and every frame pulses PA0 through U201/D202, which would wake us right back up.
	RCC->APB1RSTR |= RCC_APB1RSTR_CAN1RST | RCC_APB1RSTR_CAN2RST;
	RCC->APB1RSTR &= ~(RCC_APB1RSTR_CAN1RST | RCC_APB1RSTR_CAN2RST);

	// A pending interrupt (USB, CAN, ADC, timers...) makes WFI fall through even with IRQs
	// masked, so standby would never be entered. Disable and clear everything.
	for (size_t i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++) {
		NVIC->ICER[i] = 0xFFFFFFFF;
		NVIC->ICPR[i] = 0xFFFFFFFF;
	}
	EXTI->IMR = 0;
	EXTI->EMR = 0;
	EXTI->PR = 0xFFFFFFFF;
	SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
	PWR->CR |= PWR_CR_PDDS;
	PWR->CSR |= PWR_CSR_EWUP;

	while (true) {
		PWR->CR |= PWR_CR_CSBF | PWR_CR_CWUF;
		__DSB();
		__WFI();
	}
}

// Called by Lua mcu_standby(). R306 pulls /Power/EN low once PE10 goes high-Z;
// PA0 (key via Q201/Q202, or CAN wake via U201) wakes the MCU.
static void customBoardOnStandBy() {
	powerEnablePin.setValue(0);
	enterStandby();
}

// The F4 IWDG cannot be stopped and keeps running in standby, so ~500 ms after going to sleep
// it resets the MCU. This runs before rusEFI arms the IWDG: after an IWDG reset with no wakeup
// request (no PA0 wakeup event, PA0 low = key off and CAN idle) go straight back to standby.
// The IWDG is not running in this boot, so this time the MCU stays asleep until PA0 rises.
static void customBoardPreHalInit() {
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
	(void)RCC->AHB1ENR;

	bootPwrCsr = PWR->CSR;
	bootPa0High = (GPIOA->IDR & GPIO_IDR_IDR_0) != 0;
	bool wakeRequested = (bootPwrCsr & PWR_CSR_WUF) != 0 || bootPa0High;

	if (!wakeRequested && getMCUResetCause() == Reset_Cause_IWatchdog) {
		enterStandby();
	}

	// fresh flags for the next standby / wakeup decision
	PWR->CR |= PWR_CR_CSBF | PWR_CR_CWUF;
}

// Battery voltage is sensed on 12V_SW (key line), so it doubles as the key-on signal.
// Sleeps 5 s after a key-off; if the key was never on since boot (woken by CAN, or on
// the bench with USB only) it waits 60 s instead so the ECU stays reachable for a while.
// Never sleeps while a PC has the USB enumerated.
static const char standbyLuaScript[] = R"(
local KEY_ON_VOLTS = 6
local SLEEP_AFTER_KEY_OFF_S = 5
local SLEEP_IF_NEVER_ON_S = 60

local keyOffTimer = Timer.new()
keyOffTimer:reset()
local keyWasOn = false

setTickRate(10)

function onTick()
	local vKey = getSensor("BatteryVoltage") or 0
	if vKey > KEY_ON_VOLTS then
		keyWasOn = true
		keyOffTimer:reset()
		return
	end

	-- PC connected over USB: stay awake, restart the countdown once it is unplugged
	if getOutput("isUsbConnected") == 1 then
		keyOffTimer:reset()
		return
	end

	local limit = keyWasOn and SLEEP_AFTER_KEY_OFF_S or SLEEP_IF_NEVER_ON_S
	if keyOffTimer:getElapsedSeconds() > limit then
		print("PRO-MS6: key off, entering standby")
		mcu_standby()
	end
end
)";

// D208 blue = PE7, D207 green = PD7, D209 yellow = PE8. D206 red = PD4 is the critical LED (board.mk)
Gpio getCommsLedPin() {
	return Gpio::E7;
}

Gpio getRunningLedPin() {
	return Gpio::D7;
}

Gpio getWarningLedPin() {
	return Gpio::E8;
}

static void setupInjectors() {
	engineConfiguration->injectionPins[0] = Gpio::D3;	// J102.A4 INJ_1 (U701 BTS3046)
	engineConfiguration->injectionPins[1] = Gpio::D2;	// J102.B4 INJ_2 (U702 BTS3046)
}

static void setupIgnition() {
	// MIC4427 -> ISL9V3040 on-board igniters
	engineConfiguration->ignitionPins[0] = Gpio::C13;	// J102.M2 IGN_1
	engineConfiguration->ignitionPins[1] = Gpio::E5;	// J102.M4 IGN_2
}

static void setupTrigger() {
	engineConfiguration->triggerInputPins[0] = Gpio::E1;	// J102.F4/G4 CKP VR via MAX9924
	engineConfiguration->triggerInputPins[1] = Gpio::Unassigned;
	// No dedicated cam input: use AUX_D1 (PA15) or VVT_MAP_V_TWIN.
	// PA15 and PB15 share EXTI15 - only one of them can be an event input.
	engineConfiguration->camInputs[0] = Gpio::Unassigned;
	engineConfiguration->vehicleSpeedSensorInputPin = Gpio::E12;	// J102.H4 VSS
}

static void setupAnalogInputs() {
	// MCP6004 buffer + 4.7k/4.7k divider: 0..5V sensor -> 0..2.5V at the ADC
	engineConfiguration->analogInputDividerCoefficient = 2.0f;
	engineConfiguration->adcVcc = 3.3f;	// REF3033 on VREF+

	// R313 33k / R314 6.8k
	engineConfiguration->vbattDividerCoeff = (33.0f + 6.8f) / 6.8f;
	engineConfiguration->vbattAdcChannel = EFI_ADC_5;	// PA5 <- J102.G3 12V_SW

	engineConfiguration->map.sensor.hwChannel = EFI_ADC_10;	// PC0 <- J102.K1
	engineConfiguration->tps1_1AdcChannel = EFI_ADC_4;		// PA4 <- J102.G1
	engineConfiguration->clt.adcChannel = EFI_ADC_12;		// PC2 <- J102.F1
	engineConfiguration->iat.adcChannel = EFI_ADC_13;		// PC3 <- J102.H1

	// R410 / R414 4.7k pull-up to +5VA
	engineConfiguration->clt.config.bias_resistor = 4700;
	engineConfiguration->iat.config.bias_resistor = 4700;

	// Narrowband O2: PA1 <- J102.K2, PC1 <- J102.J2. Wideband comes over CAN from U1005.

	// Lua inputs
	engineConfiguration->auxAnalogInputs[0] = EFI_ADC_7;	// PA7 gear position ladder
	engineConfiguration->auxAnalogInputs[1] = EFI_ADC_8;	// PB0 QS_AN (J101.9)
	engineConfiguration->auxAnalogInputs[2] = EFI_ADC_3;	// PA3 AUX_AN1 (J101.1)
	engineConfiguration->auxAnalogInputs[3] = EFI_ADC_15;	// PC5 AUX_AN2 (J101.2), R208 makes this divider ~2.47
	engineConfiguration->auxAnalogInputs[4] = EFI_ADC_6;	// PA6 AUX_AN3 (J101.3)
	engineConfiguration->auxAnalogInputs[5] = EFI_ADC_14;	// PC4 AUX_AT1 (J101.15)
	engineConfiguration->auxAnalogInputs[6] = EFI_ADC_9;	// PB1 AUX_AT2 (J101.4)

	// Rev A routes FuelLevel (J101.20) to PE11, which has no ADC. Rev B moves it to PA2.
	// engineConfiguration->fuelLevelSensor = EFI_ADC_2;
}

static void setupOutputs() {
	engineConfiguration->mainRelayPin = Gpio::C8;				// J102.E1 (U706)
	engineConfiguration->fuelPumpPin = Gpio::D10;				// J102.C4 (U708)
	engineConfiguration->fanPin = Gpio::D12;					// J102.D4 (U707)
	engineConfiguration->malfunctionIndicatorPin = Gpio::D15;	// J102.K3 (U705)
	engineConfiguration->starterControlPin = Gpio::D11;			// J102.C2 (U708)
	engineConfiguration->tachOutputPin = Gpio::C10;				// J102.J4 (Q702/Q701 high side)
	engineConfiguration->o2heaterPin = Gpio::A8;				// J102.L1 (U704)
	// Free for GPPWM / Lua: PA9 J102.M1 O2_2 heater, PC6 J102.L3 canister,
	// PC7 J102.L2 SAS, PD13 J102.E4 headlight relay
}

static void setupCommunication() {
	engineConfiguration->canTxPin = Gpio::D1;	// U206 TJA1051
	engineConfiguration->canRxPin = Gpio::D0;
	engineConfiguration->enableAemXSeries = true;	// on-board rusEFI wideband (U1005) on the same bus

	engineConfiguration->is_enabled_spi_1 = true;
	engineConfiguration->spi1sckPin = Gpio::B3;
	engineConfiguration->spi1misoPin = Gpio::B4;
	engineConfiguration->spi1mosiPin = Gpio::B5;
	engineConfiguration->sdCardSpiDevice = SPI_DEVICE_1;
	engineConfiguration->sdCardCsPin = Gpio::B6;
	engineConfiguration->isSdCardEnabled = true;

	// LPS22HB, detected by lps25.cpp via WHO_AM_I 0xB1
	engineConfiguration->lps25BaroSensorScl = Gpio::B10;
	engineConfiguration->lps25BaroSensorSda = Gpio::B11;
}

// DRV8421 driven by four push-pull outputs -> J102.A1/A2/B1/B2. Defaults only: the pins are
// selectable in TunerStudio so the coil order / direction can be changed per valve.
static void setupIdleStepper() {
	engineConfiguration->useRawOutputToDriveIdleStepper = true;
	engineConfiguration->stepper_raw_output[0] = Gpio::E4;	// STEPPER_IN1
	engineConfiguration->stepper_raw_output[1] = Gpio::E3;	// STEPPER_IN2
	engineConfiguration->stepper_raw_output[2] = Gpio::E2;	// STEPPER_IN3
	engineConfiguration->stepper_raw_output[3] = Gpio::B8;	// STEPPER_IN4
}

static void setupSwitchInputs() {
	engineConfiguration->clutchDownPin = Gpio::E0;	// J102.B3
	// Lua only: PE13 neutral, PE14 side stand, PE15 roll over, PC11 mode switch, PA15/PB15 AUX_D1/D2
}

// board-specific configuration setup
static void customBoardDefaultConfiguration() {
	engineConfiguration->cylindersCount = 2;
	engineConfiguration->firingOrder = FO_1_2;

	setupInjectors();
	setupIgnition();
	setupTrigger();
	setupAnalogInputs();
	setupOutputs();
	setupCommunication();
	setupIdleStepper();
	setupSwitchInputs();

	setLuaScript(standbyLuaScript);
}

// Hard-wired to their drivers, re-applied after every load and TunerStudio write
static void customBoardConfigOverrides() {
	// IFX9201: PWM PD14, DIR PC9, DIS PB14 -> J101.8 / J101.14
	setupTLE9201(/*controlPin*/ Gpio::D14, /*direction*/ Gpio::C9, /*disable*/ Gpio::B14);
}

void setup_custom_board_overrides() {
	setupBoardHardwareTestOverrides();

	custom_board_preHalInit = customBoardPreHalInit;
	custom_board_InitHardwareEarly = customBoardInitHardwareEarly;
	custom_board_onBoardStandBy = customBoardOnStandBy;
	custom_board_DefaultConfiguration = customBoardDefaultConfiguration;
	custom_board_ConfigOverrides = customBoardConfigOverrides;
}
