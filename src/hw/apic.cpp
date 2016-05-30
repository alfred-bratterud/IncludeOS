// This file is a part of the IncludeOS unikernel - www.includeos.org
//
// Copyright 2015 Oslo and Akershus University College of Applied Sciences
// and Alfred Bratterud
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <hw/apic.hpp>
#include <hw/ioapic.hpp>
#include <hw/acpi.hpp> // ACPI
#include <hw/apic_revenant.hpp>
#include <hw/cpu.hpp>
#include <hw/ioport.hpp>
#include <hw/pic.hpp>
#include <kernel/irq_manager.hpp>
#include <cstdio>
#include <debug>
#include <info>

#define LAPIC_ID        0x20
#define LAPIC_VER       0x30
#define LAPIC_TPR       0x80
#define LAPIC_EOI       0x0B0
#define LAPIC_LDR       0x0D0
#define LAPIC_DFR       0x0E0
#define LAPIC_SPURIOUS  0x0F0
#define LAPIC_ESR	      0x280
#define LAPIC_ICRL      0x300
#define LAPIC_ICRH      0x310
#define LAPIC_LVT_TMR	  0x320
#define LAPIC_LVT_PERF  0x340
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TMRINITCNT 0x380
#define LAPIC_TMRCURRCNT 0x390
#define LAPIC_TMRDIV    0x3E0
#define LAPIC_LAST      0x38F
#define LAPIC_DISABLE   0x10000
#define LAPIC_SW_ENABLE 0x100
#define LAPIC_CPUFOCUS  0x200
#define LAPIC_NMI       (4<<8)
#define TMR_PERIODIC    0x20000
#define TMR_BASEDIV     (1<<20)

#define INTR_MASK    0x00010000

// Interrupt Command Register
#define ICR_DEST_BITS   24

// Delivery Mode
#define ICR_FIXED               0x000000
#define ICR_LOWEST              0x000100
#define ICR_SMI                 0x000200
#define ICR_NMI                 0x000400
#define ICR_INIT                0x000500
#define ICR_STARTUP             0x000600

// Destination Mode
#define ICR_PHYSICAL            0x000000
#define ICR_LOGICAL             0x000800

// Delivery Status
#define ICR_IDLE                0x000000
#define ICR_SEND_PENDING        0x001000
#define ICR_DLV_STATUS          (1u <<12)

// Level
#define ICR_DEASSERT            0x000000
#define ICR_ASSERT              0x004000

// Trigger Mode
#define ICR_EDGE                0x000000
#define ICR_LEVEL               0x008000

// Destination Shorthand
#define ICR_NO_SHORTHAND        0x000000
#define ICR_SELF                0x040000
#define ICR_ALL_INCLUDING_SELF  0x080000
#define ICR_ALL_EXCLUDING_SELF  0x0c0000

extern "C" {
  void apic_enable();
  int  get_cpu_id();
  void reboot();
  extern char _binary_apic_boot_bin_start;
  extern char _binary_apic_boot_bin_end;
  void lapic_exception_handler();
  void lapic_irq_entry();
}
extern idt_loc smp_lapic_idt;

namespace hw {
  
  static const uintptr_t IA32_APIC_BASE_MSR = 0x1B;
  static const uintptr_t IA32_APIC_BASE_MSR_ENABLE = 0x800;
  static const uintptr_t BOOTLOADER_LOCATION = 0x80000;
  
  // a single 16-byte aligned APIC register
  struct apic_reg
  {
    uint32_t volatile reg;
    uint32_t pad[3];
  };
  
  struct apic_regs
  {
    apic_reg reserved0;
    apic_reg reserved1;
    apic_reg 		lapic_id;
    apic_reg 		lapic_ver;
    apic_reg reserved4;
    apic_reg reserved5;
    apic_reg reserved6;
    apic_reg reserved7;
    apic_reg 		task_pri;               // TPRI
    apic_reg reservedb; // arb pri
    apic_reg reservedc; // cpu pri
    apic_reg 		eoi;                    // EOI
    apic_reg 		remote;
    apic_reg 		logical_dest;
    apic_reg 		dest_format;
    apic_reg 		spurious_vector;        // SIVR
    apic_reg 		isr[8];
    apic_reg 		tmr[8];
    apic_reg 		irr[8];
    apic_reg    error_status;
    apic_reg reserved28[7];
    apic_reg		intr_lo; 	    // ICR0
    apic_reg		intr_hi;      // ICR1
    apic_reg 		timer;        // LVTT
    apic_reg reserved33;
    apic_reg reserved34;      // perf count lvt
    apic_reg 		lint0;        // local interrupts (64-bit)
    apic_reg 		lint1;
    apic_reg 		error;        // error vector
    apic_reg 		init_count;   // timer
    apic_reg 		cur_count;    // timer
    apic_reg reserved3a;
    apic_reg reserved3b;
    apic_reg reserved3c;
    apic_reg reserved3d;
    apic_reg 		divider_config; // 3e, timer divider
    apic_reg reserved3f;
  };
  
  struct apic
  {
    apic() {}
    apic(uintptr_t addr)
    {
      this->regs = (apic_regs*) addr;
    }
    
    bool x2apic() const noexcept {
      return false;
    }
    
    uint32_t get_id() const noexcept {
      return (regs->lapic_id.reg >> 24) & 0xFF;
    }
    
    // set and clear one of the 255-bit registers
    void set(apic_reg* reg, uint8_t bit)
    {
      reg[bit >> 5].reg |= 1 << (bit & 0x1f);
    }
    void clr(apic_reg* reg, uint8_t bit)
    {
      reg[bit >> 5].reg &= ~(1 << (bit & 0x1f));
    }
    
    // initialize a given AP
    void ap_init(uint8_t id)
    {
      regs->intr_hi.reg = id << ICR_DEST_BITS;
      regs->intr_lo.reg = ICR_INIT | ICR_PHYSICAL
           | ICR_ASSERT | ICR_EDGE | ICR_NO_SHORTHAND;
      
      while (regs->intr_lo.reg & ICR_SEND_PENDING);
    }
    void ap_start(uint8_t id, uint32_t vector)
    {
      regs->intr_hi.reg = id << ICR_DEST_BITS;
      regs->intr_lo.reg = vector | ICR_STARTUP
        | ICR_PHYSICAL | ICR_ASSERT | ICR_EDGE | ICR_NO_SHORTHAND;
      
      while (regs->intr_lo.reg & ICR_SEND_PENDING);
    }
    
    void enable_intr(uint8_t const spurious_vector)
    {
      regs->spurious_vector.reg = 0x100 | spurious_vector;
    }
    
    apic_regs* regs;
  };
  static apic lapic;
  
  struct apic_boot {
    // the jump instruction at the start
    uint32_t   jump;
    // stuff we will need to modify
    void*  worker_addr;
    void*  stack_base;
    size_t stack_size;
  };
  
  union addr_union {
    uint32_t whole;
    uint16_t part[2];
    
    addr_union(void(*addr)()) {
      whole = (uintptr_t) addr;
    }
  };
  
  void APIC::init()
  {
    const uint64_t APIC_BASE_MSR = CPU::read_msr(IA32_APIC_BASE_MSR);
    /// find the LAPICs base address ///
    const uintptr_t APIC_BASE_ADDR = APIC_BASE_MSR & 0xFFFFF000;
    //printf("APIC base addr: 0x%x\n", APIC_BASE_ADDR);
    // acquire infos
    lapic = apic(APIC_BASE_ADDR);
    INFO2("LAPIC id: %x  ver: %x\n", lapic.get_id(), lapic.regs->lapic_ver.reg);
    
    // disable the legacy 8259 PIC
    // by masking off all interrupts
    hw::PIC::set_intr_mask(0xFFFF);
    
    // enable Local APIC
    enable();
    
    // turn the Local APIC on and enable interrupts
    INFO("APIC", "Enabling BSP LAPIC");
    CPU::write_msr(IA32_APIC_BASE_MSR, 
        (APIC_BASE_MSR & 0xfffff100) | IA32_APIC_BASE_MSR_ENABLE, 0);
    INFO2("APIC_BASE MSR is now 0x%llx\n", CPU::read_msr(IA32_APIC_BASE_MSR));
    
    // initialize I/O APICs
    IOAPIC::init(ACPI::get_ioapics());
    
    /// initialize and start APs found in ACPI-tables ///
    if (ACPI::get_cpus().size() > 1) {
      INFO("APIC", "SMP Init");
      init_smp();
    }
  }
  
  void APIC::enable()
  {
    /// enable interrupts ///
    lapic.regs->task_pri.reg       = 0xff;
    lapic.regs->dest_format.reg    = 0xffffffff; // flat mode
    lapic.regs->logical_dest.reg   = 0x01000000; // logical ID 1
    
    // hardcoded 240 + x for LAPIC interrupts
    #define LAPIC_IRQ_BASE   120
    lapic.regs->timer.reg = INTR_MASK;
    lapic.regs->lint0.reg = INTR_MASK | (LAPIC_IRQ_BASE + 3);
    lapic.regs->lint1.reg = INTR_MASK | (LAPIC_IRQ_BASE + 4);
    lapic.regs->error.reg = INTR_MASK | (LAPIC_IRQ_BASE + 5);
    
    // start receiving interrupts (0x100), set spurious vector
    // note: spurious IRQ must have 4 last bits set (0x?F)
    const uint8_t SPURIOUS_IRQ = 0x7f; // IRQ 127
    lapic.enable_intr(SPURIOUS_IRQ);
    
    // acknowledge any outstanding interrupts
    eoi();
    
    // enable APIC by resetting task priority
    lapic.regs->task_pri.reg = 0;
  }
  
  /// initialize and start registered APs found in ACPI-tables ///
  void APIC::init_smp()
  {
    // smp with only one CPU == :facepalm:
    assert(ACPI::get_cpus().size() > 1);
    
    // copy our bootloader to APIC init location
    const char* start = &_binary_apic_boot_bin_start;
    ptrdiff_t bootloader_size = &_binary_apic_boot_bin_end - start;
    debug("Copying bootloader from %p to 0x%x (size=%d)\n",
          start, BOOTLOADER_LOCATION, bootloader_size);
    memcpy((char*) BOOTLOADER_LOCATION, start, bootloader_size);
    
    // modify bootloader to support our cause
    auto* boot = (apic_boot*) BOOTLOADER_LOCATION;
    // populate IDT used with SMP LAPICs
    smp_lapic_idt.limit = 256 * sizeof(IDTDescr) - 1;
    smp_lapic_idt.base  = (uintptr_t) new IDTDescr[256];
    
    auto* idt = (IDTDescr*) smp_lapic_idt.base;
    for (size_t i = 0; i < 32; i++) {
      addr_union addr(lapic_exception_handler);
      idt[i].offset_1 = addr.part[0];
      idt[i].offset_2 = addr.part[1];
      idt[i].selector  = 0x8;
      idt[i].type_attr = 0x8e;
      idt[i].zero      = 0;
    }
    for (size_t i = 32; i < 48; i++) {
      addr_union addr(lapic_irq_entry);
      idt[i].offset_1 = addr.part[0];
      idt[i].offset_2 = addr.part[1];
      idt[i].selector  = 0x8;
      idt[i].type_attr = 0x8e;
      idt[i].zero      = 0;
    }
    
    // assign stack and main func
    size_t CPUcount = ACPI::get_cpus().size();
    
    boot->worker_addr = (void*) &revenant_main;
    boot->stack_base = aligned_alloc(CPUcount * REV_STACK_SIZE, 4096);
    boot->stack_size = REV_STACK_SIZE;
    debug("APIC stack base: %p  size: %u   main size: %u\n", 
        boot->stack_base, boot->stack_size, sizeof(boot->worker_addr));
    
    // reset barrier
    smp.boot_barrier.reset(1);
    
    // turn on CPUs
    INFO("APIC", "Initializing APs");
    for (auto& cpu : ACPI::get_cpus())
    {
      debug("-> CPU %u ID %u  fl 0x%x\n",
        cpu.cpu, cpu.id, cpu.flags);
      // except the CPU we are using now
      if (cpu.id != lapic.get_id())
        lapic.ap_init(cpu.id);
    }
    // start CPUs
    INFO("APIC", "Starting APs");
    for (auto& cpu : ACPI::get_cpus())
    {
      // except the CPU we are using now
      if (cpu.id == lapic.get_id()) continue;
      // Send SIPI with start address 0x80000
      lapic.ap_start(cpu.id, 0x80);
      lapic.ap_start(cpu.id, 0x80);
    }
    
    // wait for all APs to start
    smp.boot_barrier.spin_wait(CPUcount);
    INFO("APIC", "All APs are online now\n");
  }
  
  uint8_t APIC::get_isr()
  {
    for (uint8_t i = 0; i < 8; i++)
      if (lapic.regs->isr[i].reg)
        return 32 * i + __builtin_ffs(lapic.regs->isr[i].reg) - 1;
    return 0;
  }
  uint8_t APIC::get_irr()
  {
    for (uint8_t i = 0; i < 8; i++)
      if (lapic.regs->irr[i].reg)
        return 32 * i + __builtin_ffs(lapic.regs->irr[i].reg) - 1;
    return 0;
  }
  
  void APIC::eoi()
  {
    debug("-> eoi @ %p for %u\n", &lapic.regs->eoi.reg, lapic.get_id());
    lapic.regs->eoi.reg = 0;
  }
  
  void APIC::send_ipi(uint8_t id, uint8_t vector)
  {
    debug("send_ipi  id %u  vector %u\n", id, vector);
    // select APIC ID
    uint32_t value = lapic.regs->intr_hi.reg & 0x00ffffff;
    lapic.regs->intr_hi.reg = value | (id << 24);
    // write vector and trigger/level/mode
    value = ICR_ASSERT | ICR_FIXED | vector;
    lapic.regs->intr_lo.reg = value;
  }
  void APIC::bcast_ipi(uint8_t vector)
  {
    debug("bcast_ipi  vector %u\n", vector);
    //lapic.regs->intr_hi.reg = id << 24;
    lapic.regs->intr_lo.reg = ICR_ALL_EXCLUDING_SELF | ICR_ASSERT | vector;
  }
  
  void APIC::start_task(smp_task_func task, smp_done_func done)
  {
    smp.task_func = task;
    smp.done_func = done;
    
    smp.task_barrier.reset(1);
    bcast_ipi(0x20);
    // execute our own part of task
    task(get_cpu_id());
    // wait for all APs to finish task
    smp.task_barrier.spin_wait( ACPI::get_cpus().size() );
    // callback
    done();
  }
  
  void APIC::enable_irq(uint8_t irq)
  {
    auto& overrides = ACPI::get_overrides();
    for (auto& redir : overrides)
    {
      // NOTE: @bus_source is the IOAPIC number
      if (redir.irq_source == irq)
      {
        INFO2("Enabled redirected IRQ %u -> %u on lapic %u",
            redir.irq_source, redir.global_intr, lapic.get_id());
        IOAPIC::enable(redir.global_intr, irq, lapic.get_id());
        return;
      }
    }
    INFO2("Enabled non-redirected IRQ %u on LAPIC %u", irq, lapic.get_id());
    IOAPIC::enable(irq, irq, lapic.get_id());
  }
  void APIC::disable_irq(uint8_t irq)
  {
    IOAPIC::disable(irq);
  }
  
  void APIC::reboot()
  {
    ::reboot();
  }
}