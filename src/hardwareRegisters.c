#include <stdio.h>
#include <string.h>

#include <boolean.h>

#include "emulator.h"
#include "hardwareRegisterNames.h"
#include "hardwareRegisters.h"
#include "cpu32Opcodes.h"
#include "m68k/m68k.h"
#include "m68k/m68kcpu.h"


void checkInterrupts();


static inline uint8_t registerArrayRead8(uint32_t address){
   return palmReg[address];
}

static inline uint16_t registerArrayRead16(uint32_t address){
   return palmReg[address] << 8 | palmReg[address + 1];
}

static inline uint32_t registerArrayRead32(uint32_t address){
   return palmReg[address] << 24 | palmReg[address + 1] << 16 | palmReg[address + 2] << 8 | palmReg[address + 3];
}

static inline void registerArrayWrite8(uint32_t address, uint8_t value){
   palmReg[address] = value;
}

static inline void registerArrayWrite16(uint32_t address, uint16_t value){
   palmReg[address] = value >> 8;
   palmReg[address + 1] = value & 0xFF;
}

static inline void registerArrayWrite32(uint32_t address, uint32_t value){
   palmReg[address] = value >> 24;
   palmReg[address + 1] = (value >> 16) & 0xFF;
   palmReg[address + 2] = (value >> 8) & 0xFF;
   palmReg[address + 3] = value & 0xFF;
}


static inline void setIprIsrBit(uint32_t interruptBit){
   //allows for setting an interrupt with masking by IMR and logging in IPR
   registerArrayWrite32(IPR, registerArrayRead32(IPR) | interruptBit);
   registerArrayWrite32(ISR, registerArrayRead32(ISR) | (interruptBit & ~registerArrayRead32(IMR)));
}

static inline void clearIprIsrBit(uint32_t interruptBit){
   registerArrayWrite32(IPR, registerArrayRead32(IPR) & ~interruptBit);
   registerArrayWrite32(ISR, registerArrayRead32(ISR) & ~interruptBit);
}


static inline void checkPortDInts(){
   uint8_t requestedRow = registerArrayRead8(PKDIR) & registerArrayRead8(PKDATA);//keys are requested on port k and read on port d
   uint8_t portDValue = 0x00;//ports always read the chip pins even if they are set to output
   uint8_t portDpolarity = registerArrayRead8(PDPOL);
   uint8_t portDIrqEnable = registerArrayRead8(PDIRQEN);
   uint8_t portDIrqPins = ~registerArrayRead8(PDSEL);
   
   portDValue |= 0x80/*battery not dead bit*/;
   
   if(palmSdCard.inserted){
      portDValue |= 0x20;
   }
   
   if((requestedRow & 0x20) == 0){
      //kbd row 0
      portDValue |= (!palmIo.buttonCalender | (!palmIo.buttonAddress << 1) | (!palmIo.buttonTodo << 2) | (!palmIo.buttonNotes << 3)) ^ portDpolarity;
   }
   
   if((requestedRow & 0x40) == 0){
      //kbd row 1
      portDValue |= (!palmIo.buttonUp | (!palmIo.buttonDown << 1)) ^ portDpolarity;
   }
   
   if((requestedRow & 0x80) == 0){
      //kbd row 2
      portDValue |= (!palmIo.buttonPower | (!palmIo.buttonContrast << 1) | (!palmIo.buttonAddress << 3)) ^ portDpolarity;
   }
   
   if(portDIrqEnable & portDValue & 0x01){
      //int 0
      setIprIsrBit(INT_INT0);
   }
   
   if(portDIrqEnable & portDValue & 0x02){
      //int 1
      setIprIsrBit(INT_INT1);
   }
   
   if(portDIrqEnable & portDValue & 0x04){
      //int 2
      setIprIsrBit(INT_INT2);
   }
   
   if(portDIrqEnable & portDValue & 0x08){
      //int 3
      setIprIsrBit(INT_INT3);
   }
   
   if(portDIrqPins & portDValue & 0x10){
      //irq 1
      setIprIsrBit(INT_IRQ1);
   }
   
   if(portDIrqPins & portDValue & 0x20){
      //irq 2
      setIprIsrBit(INT_IRQ2);
   }
   
   if(portDIrqPins & portDValue & 0x40){
      //irq 3
      setIprIsrBit(INT_IRQ3);
   }
   
   if(portDIrqPins & portDValue & 0x80){
      //irq 6
      setIprIsrBit(INT_IRQ6);
   }
   
   checkInterrupts();
}

static inline uint8_t getPortDValue(){
   uint8_t requestedRow = registerArrayRead8(PKDIR) & registerArrayRead8(PKDATA);//keys are requested on port k and read on port d
   uint8_t portDValue = 0x00;//ports always read the chip pins even if they are set to output
   uint8_t portDpolarity = registerArrayRead8(PDPOL);
   uint8_t portDIrqEnable = registerArrayRead8(PDIRQEN);
   
   portDValue |= 0x80/*battery not dead bit*/;
   
   if(palmSdCard.inserted){
      portDValue |= 0x20;
   }
   
   if((requestedRow & 0x20) == 0){
      //kbd row 0
      portDValue |= (!palmIo.buttonCalender | (!palmIo.buttonAddress << 1) | (!palmIo.buttonTodo << 2) | (!palmIo.buttonNotes << 3)) ^ portDpolarity;
   }
   
   if((requestedRow & 0x40) == 0){
      //kbd row 1
      portDValue |= (!palmIo.buttonUp | (!palmIo.buttonDown << 1)) ^ portDpolarity;
   }
   
   if((requestedRow & 0x80) == 0){
      //kbd row 2
      portDValue |= (!palmIo.buttonPower | (!palmIo.buttonContrast << 1) | (!palmIo.buttonAddress << 3)) ^ portDpolarity;
   }
   
   return portDValue;
}


uint32_t clk32Counter;
double timer1CycleCounter;
double timer2CycleCounter;


void printUnknownHwAccess(unsigned int address, unsigned int value, unsigned int size, bool isWrite){
   if(isWrite){
      printf("Cpu Wrote %d bits of 0x%08X to register 0x%04X.\n", size, value, address);
   }
   else{
      printf("Cpu Read %d bits from register 0x%04X.\n", size, address);
   }
}

static inline void setPllfsr16(uint16_t value){
   if(!(registerArrayRead16(PLLFSR) & 0x4000)){
      //frequency protect bit not set
      registerArrayWrite16(PLLFSR, value & 0x4FFF);
      double prescaler1 = (registerArrayRead16(PLLCR) & 0x0080) ? 2 : 1;
      double p = value & 0x00FF;
      double q = (value & 0x0F00) >> 8;
      palmCrystalCycles = 2.0 * (14.0 * (p + 1.0) + q + 1.0) / prescaler1;
      printf("New CPU frequency of:%f cycles per second.\n", CPU_FREQUENCY);
      printf("New clk32 cycle count of :%f.\n", palmCrystalCycles);
   }
}

static inline void setPllcr(uint16_t value){
   //values that matter are disable pll, prescaler 1 and possibly wakeselect
   registerArrayWrite16(PLLCR, value & 0x3FBB);
   uint16_t pllfsr = registerArrayRead16(PLLFSR);
   double prescaler1 = (value & 0x0080) ? 2 : 1;
   double p = pllfsr & 0x00FF;
   double q = (pllfsr & 0x0F00) >> 8;
   palmCrystalCycles = 2.0 * (14.0 * (p + 1.0) + q + 1.0) / prescaler1;
   printf("New CPU frequency of:%f cycles per second.\n", CPU_FREQUENCY);
   printf("New clk32 cycle count of :%f.\n", palmCrystalCycles);
   
   if(value & 0x0008){
      //the pll is disabled, the cpu is off, end execution now
      m68k_end_timeslice();
   }
}

static inline double dmaclksPerClk32(){
   uint16_t pllcr = registerArrayRead16(PLLCR);
   double   dmaclks = palmCrystalCycles;
   
   if(pllcr & 0x0080){
      //prescaler 1 enabled, divide by 2
      dmaclks /= 2.0;
   }
   
   if(pllcr & 0x0020){
      //prescaler 2 enabled, divides value from prescaler 1 by 2
      dmaclks /= 2.0;
   }
   
   return dmaclks;
}

static inline double sysclksPerClk32(){
   uint16_t pllcr = registerArrayRead16(PLLCR);
   double   sysclks = dmaclksPerClk32();
   uint16_t sysclkSelect = (pllcr >> 8) & 0x0003;
   
   switch(sysclkSelect){
         
      case 0x0000:
         sysclks /= 2.0;
         break;
         
      case 0x0001:
         sysclks /= 4.0;
         break;
         
      case 0x0002:
         sysclks /= 8.0;
         break;
         
      case 0x0003:
         sysclks /= 16.0;
         break;
         
      default:
         //no divide for 0x0004, 0x0005, 0x0006 or 0x0007
         break;
   }
   
   return sysclks;
}

static inline void rtiInterruptClk32(){
   //this function is part of clk32();
   uint16_t triggeredRtiInterrupts = 0;
   
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 512) == 0){
      //RIS7 - 512HZ
      triggeredRtiInterrupts |= 0x8000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 256) == 0){
      //RIS6 - 256HZ
      triggeredRtiInterrupts |= 0x4000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 128) == 0){
      //RIS5 - 128HZ
      triggeredRtiInterrupts |= 0x2000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 64) == 0){
      //RIS4 - 64HZ
      triggeredRtiInterrupts |= 0x1000;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 32) == 0){
      //RIS3 - 32HZ
      triggeredRtiInterrupts |= 0x0800;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 16) == 0){
      //RIS2 - 16HZ
      triggeredRtiInterrupts |= 0x0400;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 8) == 0){
      //RIS1 - 8HZ
      triggeredRtiInterrupts |= 0x0200;
   }
   if(clk32Counter % ((uint32_t)CRYSTAL_FREQUENCY / 4) == 0){
      //RIS0 - 4HZ
      triggeredRtiInterrupts |= 0x0100;
   }
   
   triggeredRtiInterrupts &= registerArrayRead16(RTCIENR);
   if(triggeredRtiInterrupts){
      registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) | triggeredRtiInterrupts);
      setIprIsrBit(INT_RTI);
   }
}

static inline void timer12Clk32(){
   //this function is part of clk32();
   uint16_t timer1Control = registerArrayRead16(TCTL1);
   uint16_t timer1Prescaler = registerArrayRead16(TPRER1) & 0x00FF;
   uint16_t timer1Compare = registerArrayRead16(TCMP1);
   uint16_t timer1Count = registerArrayRead16(TCN1);
   
   uint16_t timer2Control = registerArrayRead16(TCTL2);
   uint16_t timer2Prescaler = registerArrayRead16(TPRER2) & 0x00FF;
   uint16_t timer2Compare = registerArrayRead16(TCMP2);
   uint16_t timer2Count = registerArrayRead16(TCN2);
   
   //timer 1
   if(timer1Control & 0x0001){
      //enabled
      switch((timer1Control & 0x000E) >> 1){
            
         case 0x0000://stop counter
         case 0x0003://TIN pin / timer prescaler, nothing is attached to TIN
            //do nothing
            break;
            
         case 0x0001://sysclk / timer prescaler
            timer1CycleCounter += sysclksPerClk32() / (double)timer1Prescaler;
            break;
            
         case 0x0002://sysclk / 16 / timer prescaler
            timer1CycleCounter += sysclksPerClk32() / 16.0 / (double)timer1Prescaler;
            break;
            
         default://clk32 / timer prescaler
            timer1CycleCounter += 1.0 / (double)timer1Prescaler;
            break;
      }
      
      if(timer1CycleCounter >= 1.0){
         timer1Count += (uint16_t)timer1CycleCounter;
         timer1CycleCounter -= (uint16_t)timer1CycleCounter;
      }
      
      if(timer1Count == timer1Compare){
         if(timer1Control & 0x0010){
            //interrupt enabled
            setIprIsrBit(INT_TMR1);
         }
         
         if(!(timer1Control & 0x0100)){
            //not free running, reset to 0
            timer1Count = 0x0000;
         }
      }
      
      registerArrayWrite16(TCN1, timer1Count);
   }
   
   //timer 2
   if(timer2Control & 0x0001){
      //enabled
      switch((timer2Control & 0x000E) >> 1){
            
         case 0x0000://stop counter
         case 0x0003://TIN pin / timer prescaler, nothing is attached to TIN
            //do nothing
            break;
            
         case 0x0001://sysclk / timer prescaler
            timer2CycleCounter += sysclksPerClk32() / (double)timer2Prescaler;
            break;
            
         case 0x0002://sysclk / 16 / timer prescaler
            timer2CycleCounter += sysclksPerClk32() / 16.0 / (double)timer2Prescaler;
            break;
            
         default://clk32 / timer prescaler
            timer2CycleCounter += 1.0 / (double)timer2Prescaler;
            break;
      }
      
      if(timer2CycleCounter >= 1.0){
         timer2Count += (uint16_t)timer2CycleCounter;
         timer2CycleCounter -= (uint16_t)timer2CycleCounter;
      }
      
      if(timer2Count == timer2Compare){
         if(timer2Control & 0x0010){
            //interrupt enabled
            setIprIsrBit(INT_TMR2);
         }
         
         if(!(timer2Control & 0x0100)){
            //not free running, reset to 0
            timer2Count = 0x0000;
         }
      }
      
      registerArrayWrite16(TCN2, timer2Count);
   }
}

void checkInterrupts(){
   uint32_t activeInterrupts = registerArrayRead32(ISR);
   uint16_t interruptLevelControlRegister = registerArrayRead16(ILCR);
   uint32_t intLevel = 0;
   
   if(activeInterrupts & INT_EMIQ){
      //EMIQ - Emulator Irq, has nothing to do with emulation, used for debugging on a dev board
      intLevel = 7;
   }
   
   /*
    if(activeInterrupts & INT_RTI){
    //RTI - Real Time Interrupt
    if(intLevel < 4)
    intLevel = 4;
    }
    */
   
   if(activeInterrupts & INT_SPI1){
      uint32_t spi1IrqLevel = interruptLevelControlRegister >> 12;
      if(intLevel < spi1IrqLevel)
         intLevel = spi1IrqLevel;
   }
   
   if(activeInterrupts & INT_IRQ5){
      if(intLevel < 5)
         intLevel = 5;
   }
   
   /*
    if(activeInterrupts & INT_IRQ6){
    if(intLevel < 6)
    intLevel = 6;
    }
    */
   
   if(activeInterrupts & INT_IRQ3){
      if(intLevel < 3)
         intLevel = 3;
   }
   
   if(activeInterrupts & INT_IRQ2){
      if(intLevel < 2)
         intLevel = 2;
   }
   
   if(activeInterrupts & INT_IRQ1){
      if(intLevel < 1)
         intLevel = 1;
   }
   
   if(activeInterrupts & INT_PWM2){
      uint32_t pwm2IrqLevel = (interruptLevelControlRegister >> 4) & 0x0007;
      if(intLevel < pwm2IrqLevel)
         intLevel = pwm2IrqLevel;
   }
   
   if(activeInterrupts & INT_UART2){
      uint32_t uart2IrqLevel = (interruptLevelControlRegister >> 8) & 0x0007;
      if(intLevel < uart2IrqLevel)
         intLevel = uart2IrqLevel;
   }
   
   /*
    if(activeInterrupts & INT_INT3){
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_INT2){
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_INT1){
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_INT0){
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_PWM1){
    if(intLevel < 6)
    intlevel = 6;
    }
    
    if(activeInterrupts & INT_KB){
    //KB - Keyboard
    if(intLevel < 4)
    intLevel = 4;
    }
    */
   
   if(activeInterrupts & INT_TMR2){
      //TMR2 - Timer 2
      uint32_t timer2IrqLevel = interruptLevelControlRegister & 0x0007;
      if(intLevel < timer2IrqLevel)
         intLevel = timer2IrqLevel;
   }
   
   /*
    if(activeInterrupts & INT_RTC){
    //RTC - Real Time Clock
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_WDT){
    //WDT - Watchdog Timer
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_UART1){
    if(intLevel < 4)
    intLevel = 4;
    }
    
    if(activeInterrupts & INT_TMR1){
    //TMR1 - Timer 1
    if(intLevel < 6)
    intLevel = 6;
    }
    
    if(activeInterrupts & INT_SPI2){
    if(intLevel < 4)
    intLevel = 4;
    }
    */
   
   if(activeInterrupts & (INT_TMR1 | INT_PWM1 | INT_IRQ6)){
      //All Fixed Level 6 Interrupts
      if(intLevel < 6)
         intLevel = 6;
   }
   
   if(activeInterrupts & (INT_SPI2 | INT_UART1 | INT_WDT | INT_RTC | INT_KB | INT_INT0 | INT_INT1 | INT_INT2 | INT_INT3 | INT_RTI)){
      //All Fixed Level 4 Interrupts
      if(intLevel < 4)
         intLevel = 4;
   }
   
   /*
    if(lowPowerStopActive){
    uint32_t intMask = (m68k_get_reg(NULL, M68K_REG_SR) >> 8) & 0x0007;
    if(intLevel == 7 || intLevel > intMask)
    lowPowerStopActive = false;
    }
    */
   
   if(intLevel != 0)
      m68k_set_irq(intLevel);
}


static inline void rtcAddSecondClk32(){
   //this function is part of clk32();
   
   //rtc
   if(registerArrayRead16(RTCCTL) & 0x0080){
      //rtc enable bit set
      uint16_t rtcInterruptEvents;
      uint32_t newRtcTime;
      uint32_t oldRtcTime = registerArrayRead32(RTCTIME);
      uint32_t hours = oldRtcTime >> 24;
      uint32_t minutes = (oldRtcTime >> 16) & 0x0000003F;
      uint32_t seconds = oldRtcTime & 0x0000003F;
      
      seconds++;
      rtcInterruptEvents = 0x0010;//1 second interrupt
      if(seconds >= 60){
         minutes++;
         seconds = 0;
         rtcInterruptEvents |= 0x0002;//1 minute interrupt
         if(minutes >= 60){
            hours++;
            minutes = 0;
            rtcInterruptEvents |= 0x0020;//1 hour interrupt
            if(hours >= 24){
               hours = 0;
               uint16_t days = registerArrayRead16(DAYR);
               days++;
               registerArrayWrite16(DAYR, days & 0x01FF);
               rtcInterruptEvents |= 0x0008;//1 day interrupt
            }
         }
      }
      
      rtcInterruptEvents &= registerArrayRead16(RTCIENR);
      if(rtcInterruptEvents){
         registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) | rtcInterruptEvents);
         setIprIsrBit(INT_RTC);
      }
      
      newRtcTime = seconds & 0x0000003F;
      newRtcTime |= minutes << 16;
      newRtcTime |= hours << 24;
      registerArrayWrite32(RTCTIME, newRtcTime);
   }
   
   //watchdog
   uint16_t watchdogState = registerArrayRead16(WATCHDOG);
   if(watchdogState & 0x0001){
      //watchdog enabled
      watchdogState += 0x0100;//add second to watchdog timer
      watchdogState &= 0x0383;//cap overflow
      if((watchdogState & 0x0200) == 0x0200){
         //time expired
         if(watchdogState & 0x0002){
            //interrupt
            setIprIsrBit(INT_WDT);
         }
         else{
            //reset
            emulatorReset();
            return;
         }
      }
      registerArrayWrite16(WATCHDOG, watchdogState);
   }
}

void clk32(){
   registerArrayWrite16(PLLFSR, registerArrayRead16(PLLFSR) ^ 0x8000);
   //rolls over every second
   if(clk32Counter >= CRYSTAL_FREQUENCY - 1){
      clk32Counter = 0;
      rtcAddSecondClk32();
   }
   else{
      clk32Counter++;
   }
   
   rtiInterruptClk32();
   timer12Clk32();
   
   checkInterrupts();
}

bool cpuIsOn(){
   return !((registerArrayRead16(PLLCR) & 0x0008) || lowPowerStopActive);
}

int interruptAcknowledge(int intLevel){
   int vectorOffset = registerArrayRead8(IVR);
   int vector;
   
   //If an interrupt occurs before the IVR has been programmed, the interrupt vector number 0x0F is returned to the CPU as an uninitialized interrupt.
   if(!vectorOffset)
      vector = EXCEPTION_UNINITIALIZED_INTERRUPT;
   else
      vector = vectorOffset | intLevel;
   
   //dont know if interrupts reenable the pll if disabled, but they exit low power stop mode
   lowPowerStopActive = false;
   
   //the interrupt is not really cleared until the handler writes a 1 to its source register, if the os doesnt
   //clear the interrupt properly it will trigger again on the first clk32 after returning from this interrupt
   CPU_INT_LEVEL = 0;
   
   return vector;
}

bool sed1376ClockConnected(){
   //this is the clock output pin for the sed1376, if its disabled so is the lcd controller
   return !(registerArrayRead8(PFSEL) & 0x04);
}


unsigned int getHwRegister8(unsigned int address){
   switch(address){
         
      case PADATA:
         return getPortDValue();
         
      //select between gpio or special function
      case PBSEL:
      case PCSEL:
      case PDSEL:
      case PESEL:
      case PFSEL:
      case PGSEL:
      case PJSEL:
      case PKSEL:
      case PMSEL:
         
      //pull up/down enable
      case PAPUEN:
      case PBPUEN:
      case PCPDEN:
      case PDPUEN:
      case PEPUEN:
      case PFPUEN:
      case PGPUEN:
      case PJPUEN:
      case PKPUEN:
      case PMPUEN:
         //simple read, no actions needed
         //PGPUEN, PGSEL PMSEL and PMPUEN lack the top 2 bits but that is handled on write
         //PDSEL lacks the bottom 4 bits but that is handled on write
         return registerArrayRead8(address);
         
      default:
         printUnknownHwAccess(address, 0, 8, false);
         return registerArrayRead8(address);
   }
}

unsigned int getHwRegister16(unsigned int address){
   switch(address){
         
      case PLLCR:
      case PLLFSR:
      case SDCTRL:
      case RTCISR:
      case RTCCTL:
      case RTCIENR:
         //simple read, no actions needed
         return registerArrayRead16(address);
         
      default:
         printUnknownHwAccess(address, 0, 16, false);
         return registerArrayRead16(address);
   }
}

unsigned int getHwRegister32(unsigned int address){
   switch(address){
         
      case ISR:
      case IPR:
      case IMR:
      case RTCTIME:
      case IDR:
         //simple read, no actions needed
         return registerArrayRead32(address);
         
      default:
         printUnknownHwAccess(address, 0, 32, false);
         return registerArrayRead32(address);
   }
}


void setHwRegister8(unsigned int address, unsigned int value){
   switch(address){
         
      case PKDIR:
      case PKDATA:
         checkPortDInts();
         break;
         
      case PDSEL:
         //write without the bottom 4 bits
         registerArrayWrite8(address, value & 0xF0);
         break;
         
      case PDPOL:
      case PDIRQEN:
      case PDIRQEG:
         //write without the top 4 bits
         registerArrayWrite8(address, value & 0x0F);
         break;
         
      case IVR:
         //write without the bottom 3 bits
         registerArrayWrite8(address, value & 0xF8);
         break;
         
      case PGSEL:
      case PMSEL:
      case PGPUEN:
      case PMPUEN:
      case PGDIR:
      case PMDIR:
         //write without the top 2 bits
         registerArrayWrite8(address, value & 0x3F);
         break;
         
      //select between gpio or special function
      case PBSEL:
      case PCSEL:
      case PESEL:
      case PFSEL:
      case PJSEL:
      case PKSEL:
      
      //direction select
      case PADIR:
      case PBDIR:
      case PCDIR:
      case PDDIR:
      case PEDIR:
      case PFDIR:
      case PJDIR:
      
      //pull up/down enable
      case PAPUEN:
      case PBPUEN:
      case PCPDEN:
      case PDPUEN:
      case PEPUEN:
      case PFPUEN:
      case PJPUEN:
      case PKPUEN:
         //simple write, no actions needed
         registerArrayWrite8(address, value);
         break;
         
      default:
         printUnknownHwAccess(address, value, 8, true);
         registerArrayWrite8(address, value);
         break;
   }
}

void setHwRegister16(unsigned int address, unsigned int value){
   switch(address){
         
      case RTCIENR:
         //missing bits 6 and 7
         registerArrayWrite16(address, value & 0xFF3F);
         break;
         
      case IMR:
         //this is a 32 bit register but palm os writes it as 16 bit chunks
         registerArrayWrite16(address, value & 0x00FF);
         break;
      case IMR + 2:
         //this is a 32 bit register but palm os writes it as 16 bit chunks
         registerArrayWrite16(address, value & 0x03FF);
         break;
         
      case ISR + 2:
         //this is a 32 bit register but palm os writes it as 16 bit chunks
         registerArrayWrite16(ISR + 2, registerArrayRead16(ISR + 2) & ~(value & 0x0F00));
         break;
         
      case TCTL1:
      case TCTL2:
         registerArrayWrite16(address, value & 0x01FF);
         break;
         
      case WATCHDOG:
         //writing to the watchdog resets the counter bits(8 and 9) to 0
         registerArrayWrite16(address, value & 0x0083);
         break;
         
      case RTCISR:
         registerArrayWrite16(RTCISR, registerArrayRead16(RTCISR) & ~value);
         if(!(registerArrayRead16(RTCISR) & 0xFF00))
            clearIprIsrBit(INT_RTI);
         if(!(registerArrayRead16(RTCISR) & 0x003F))
            clearIprIsrBit(INT_RTC);
         break;
         
      case PLLFSR:
         setPllfsr16(value);
         break;
         
      case PLLCR:
         setPllcr(value);
         break;
         
      case ICR:
         //missing bottom 7 bits
         registerArrayWrite16(address, value & 0xFF80);
         break;
         
      case SDCTRL:
         //missing bits 13, 9, 8 and 7
         registerArrayWrite16(address, value & 0xDC7F);
         break;
         
      default:
         printUnknownHwAccess(address, value, 16, true);
         registerArrayWrite16(address, value);
         break;
   }
}

void setHwRegister32(unsigned int address, unsigned int value){
   switch(address){
         
      case RTCTIME:
         registerArrayWrite32(address, value & 0x1F3F003F);
         break;
         
      case IDR:
      case IPR:
         //write to read only register, do nothing
         break;
         
      case ISR:
         //clear ISR and IPR for external hardware whereever there is a 1 bit in value
         registerArrayWrite32(IPR, registerArrayRead32(IPR) & ~(value & 0x000F0F00/*external hardware int mask*/));
         registerArrayWrite32(ISR, registerArrayRead32(ISR) & ~(value & 0x000F0F00/*external hardware int mask*/));
         break;
         
      case IMR:
         registerArrayWrite32(address, value & 0x00FF3FFF);
         break;
         
      case LSSA:
         //simple write, no actions needed
         registerArrayWrite32(address, value);
         break;
      
      default:
         printUnknownHwAccess(address, value, 32, true);
         registerArrayWrite32(address, value);
         break;
   }
}


void resetHwRegisters(){
   memset(palmReg, 0x00, REG_SIZE);
   clk32Counter = 0;
   timer1CycleCounter = 0.0;
   timer2CycleCounter = 0.0;
   
   //system control
   registerArrayWrite8(SCR, 0x1C);
   
   //cpu id
   registerArrayWrite32(IDR, 0x56000000);
   
   //i/o drive control //probably unused
   registerArrayWrite16(IODCR, 0x1FFF);
   
   //chip selects
   registerArrayWrite16(CSA, 0x00B0);
   registerArrayWrite16(CSD, 0x0200);
   registerArrayWrite16(EMUCS, 0x0060);
   
   //phase lock loop
   registerArrayWrite16(PLLCR, 0x24B3);
   registerArrayWrite16(PLLFSR, 0x0347);
   
   //power control
   registerArrayWrite8(PCTLR, 0x1F);
   
   //cpu interrupts
   registerArrayWrite32(IMR, 0x00FFFFFF);
   registerArrayWrite16(ILCR, 0x6533);
   
   //gpio ports
   registerArrayWrite8(PADATA, 0xFF);
   registerArrayWrite8(PAPUEN, 0xFF);
   
   registerArrayWrite8(PBDATA, 0xFF);
   registerArrayWrite8(PBPUEN, 0xFF);
   registerArrayWrite8(PBSEL, 0xFF);
   
   registerArrayWrite8(PCPDEN, 0xFF);
   registerArrayWrite8(PCSEL, 0xFF);
   
   registerArrayWrite8(PDDATA, 0xFF);
   registerArrayWrite8(PDPUEN, 0xFF);
   registerArrayWrite8(PDSEL, 0xF0);
   
   registerArrayWrite8(PEDATA, 0xFF);
   registerArrayWrite8(PEPUEN, 0xFF);
   registerArrayWrite8(PESEL, 0xFF);
   
   registerArrayWrite8(PFDATA, 0xFF);
   registerArrayWrite8(PFPUEN, 0xFF);
   registerArrayWrite8(PFSEL, 0x87);
   
   registerArrayWrite8(PGDATA, 0x3F);
   registerArrayWrite8(PGPUEN, 0x3D);
   registerArrayWrite8(PGSEL, 0x08);
   
   registerArrayWrite8(PJDATA, 0xFF);
   registerArrayWrite8(PJPUEN, 0xFF);
   registerArrayWrite8(PJSEL, 0xEF);
   
   registerArrayWrite8(PKDATA, 0x0F);
   registerArrayWrite8(PKPUEN, 0xFF);
   registerArrayWrite8(PKSEL, 0xFF);
   
   registerArrayWrite8(PMDATA, 0x20);
   registerArrayWrite8(PMPUEN, 0x3F);
   registerArrayWrite8(PMSEL, 0x3F);
   
   //pulse width modulation control
   registerArrayWrite16(PWMC1, 0x0020);
   registerArrayWrite8(PWMP1, 0xFE);
   
   //timers
   registerArrayWrite16(TCMP1, 0xFFFF);
   registerArrayWrite16(TCMP2, 0xFFFF);
   
   //serial i/o
   registerArrayWrite16(UBAUD1, 0x003F);
   registerArrayWrite16(UBAUD2, 0x003F);
   registerArrayWrite16(HMARK, 0x0102);
   
   //lcd control registers, unused since the sed1376 is present
   registerArrayWrite8(LVPW, 0xFF);
   registerArrayWrite16(LXMAX, 0x03F0);
   registerArrayWrite16(LYMAX, 0x01FF);
   registerArrayWrite16(LCWCH, 0x0101);
   registerArrayWrite8(LBLKC, 0x7F);
   registerArrayWrite8(LRRA, 0xFF);
   registerArrayWrite8(LGPMR, 0x84);
   registerArrayWrite8(DMACR, 0x62);
   
   //realtime clock
   //RTCTIME is not changed on reset
   registerArrayWrite16(WATCHDOG, 0x0001);
   registerArrayWrite16(RTCCTL, 0x0080);//conflicting size in datasheet, it says its 8 bit but provides 16 bit values
   registerArrayWrite16(STPWCH, 0x003F);//conflicting size in datasheet, it says its 8 bit but provides 16 bit values
   //DAYR is not changed on reset
   
   //sdram control, unused since ram refresh is unemulated
   registerArrayWrite16(SDCTRL, 0x003C);
}

void setRtc(uint32_t days, uint32_t hours, uint32_t minutes, uint32_t seconds){
   uint32_t rtcTime;
   rtcTime = seconds & 0x0000003F;
   rtcTime |= (minutes << 16) & 0x003F0000;
   rtcTime |= (hours << 24) & 0x1F000000;
   registerArrayWrite32(RTCTIME, rtcTime);
   registerArrayWrite16(DAYR, days & 0x01FF);
}