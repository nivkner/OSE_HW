#include <inc/string.h>
#include <inc/error.h>
#include <kern/env.h>
#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/picirq.h>
#include <kern/sched.h>

typedef uint32_t reg_t;

#define TX_DESC_COUNT 16
#define RX_DESC_COUNT 128

// calculates the number of unused registers,
// between two byte offsets
#define UNUSED_BETWEEN(REG1, REG2) ((REG2 - REG1 - sizeof(reg_t)) / sizeof(reg_t))

// defines a new MMIO register field with the given type, name and offset,
// and pads the structure untill the next used register
#define ADD_REG(name, offset, next) reg_t name; \
    reg_t _unused_after_##name[UNUSED_BETWEEN(offset, next)];

#define E1000_CTRL      0x00000
#define E1000_STATUS    0x00008
#define E1000_EERD      0x00014
#define E1000_ICR       0x000C0
#define E1000_ICS       0x000C8
#define E1000_IMS       0x000D0
#define E1000_IMC       0x000D8
#define E1000_RCTL      0x00100
#define E1000_TCTL      0x00400
#define E1000_TIPG      0x00410
#define E1000_RDBAL     0x02800
#define E1000_RDBAH     0x02804
#define E1000_RDLEN     0x02808
#define E1000_RDH       0x02810
#define E1000_RDT       0x02818
#define E1000_RSRPD     0x02C00
#define E1000_TDBAL     0x03800
#define E1000_TDBAH     0x03804
#define E1000_TDLEN     0x03808
#define E1000_TDH       0x03810
#define E1000_TDT       0x03818
#define E1000_TIDV      0x03820
#define E1000_MTA       0x05200
#define E1000_RAL0      0x05400
#define E1000_RAH0      0x05404

// RCTL Register
#define RCTL_EN             (1 << 1)   // Receiver Enable
#define RCTL_BAM            (1 << 15)  // Broadcast Accept Mode
#define RCTL_BSIZE_shift    16         // Receive Buffer Size
#define RCTL_BSEX           (1 << 25)  // Buffer Size Extension
#define RCTL_SECRC          (1 << 26)  // Strip Ethernet CRC


// RCTL Register setup values
#define RCTL_BSIZE       0

// Receive Address Registers
#define RA_HIGH_MASK        0xFFFF     // Mask bits for high receive address
#define RA_HIGH_AV          (1 << 31)  // Address Valid

// Reception Status
#define RX_STATUS_DD    1           // Descriptor Done
#define RX_STATUS_EOP   (1 << 1)    // End of Packet

// TCTL Register
#define TCTL_EN         (1 << 1)    // Transmit Enable
#define TCTL_PSP        (1 << 3)    // Pad Short Packets
#define TCTL_CT_SHIFT   4           // Collision Threshold
#define TCTL_COLD_SHIFT 12          // Collision Distance
#define TCTL_SWXOFF     (1 << 22)   // Software XOFF Transmission

// TCTL Register setup values
#define TCTL_CT         (0x10 << TCTL_CT_SHIFT)
#define TCTL_COLD       (0x40 << TCTL_COLD_SHIFT)

// TIPG Register
#define TIPG_IPGT_SHIFT     0       // IPG Transmit Time
#define TIPG_IPGR1_SHIFT    10      // IPG Receive Time 1
#define TIPG_IPGR2_SHIFT    20      // IPG Receive Time 2

// TIPG Register setup values
// based on section 13.4.34 of the Developer's Manual
//
// this section contains a contradition
// since the value of IPGR2 if 6, 2/3 of which is 4, not 8.
//
// the answer is given by cross-referencing with Intel Ethernet Controller I217 datasheet,
// which states that for IEEE 802.3 compliance, IPGR1 should be 2/3 of the total effective IPG
// which is 12 once the additional 6 MAC clocks used by the MAC are added to IPGR2.
#define TIPG_IPGT           (10 << TIPG_IPGT_SHIFT)
#define TIPG_IPGR1          (8 << TIPG_IPGR1_SHIFT)
#define TIPG_IPGR2          (20 << TIPG_IPGR2_SHIFT)

// Trasmission status
#define TX_STATUS_DD        1           // Descriptor Done
#define TX_STATUS_EC        (1 << 1)    // Excess Collisions
#define TX_STATUS_LC        (1 << 2)    // Late Collision
#define TX_STATUS_TU        (1 << 3)    // Transmit Underrun

// Trasmission Command
#define TX_CMD_EOP       1           // End Of Packet
#define TX_CMD_IFCS      (1 << 1)    // Insert FCS
#define TX_CMD_IC        (1 << 2)    // Insert Checksum
#define TX_CMD_RS        (1 << 3)    // Report Status
#define TX_CMD_RSV       (1 << 4)    // Report Packet Sent
#define TX_CMD_DEXT      (1 << 5)    // Extension (0 for legacy mode)
#define TX_CMD_VLE       (1 << 6)    // VLAN Packet Enable
#define TX_CMD_IDE       (1 << 7)    // Interrupt Delay Enable

// Interrupt Mask
#define INT_TXDW        1           // Transmit Descriptor Written Back
#define INT_TXQE        (1 << 1)    // Transmit Queue Empty
#define INT_RXDMT0      (1 << 4)    // Receive Descriptor Minimum Threshold hit
#define INT_SRPD        (1 << 16)   // Small Receive Packet Detection

// Interrupt Cause Read 
#define ICR_TXDW          1          // Transmit desc written back
#define ICR_TXQE          (1 << 1)   // Transmit Queue empty
#define ICR_LSC           (1 << 2)   // Link Status Change
#define ICR_RXSEQ         (1 << 3)   // Receive Sequence Error
#define ICR_RXDMT0        (1 << 4)   // Receive Descriptor Minimum Threshold Reached
#define ICR_RXO           (1 << 6)   // Receiver Overrun
#define ICR_RXT0          (1 << 7)   // Receiver Timer Interrupt
#define ICR_TXD_LOW       (1 << 15)  // Transmit Descriptor Low Threshold hit.
#define ICR_SRPD          (1 << 16)  // Small Receive Packet Detected

// EEPROM Read Register
#define EERD_START          1
#define EERD_DONE           (1 << 4)
#define EERD_ADDR_SHIFT     8
#define EERD_DATA_SHIFT     16
#define EERD_DATA_MASK      0xFFFF

struct e1000_regs {
    // Device Control - RW
    ADD_REG(ctrl, E1000_CTRL, E1000_STATUS)
    // Device Status - RO
    ADD_REG(status, E1000_STATUS, E1000_EERD)
    // EEPROM Read - RW
    ADD_REG(eerd, E1000_EERD, E1000_ICR)
    // Interrupt Cause Read - R/clr
    ADD_REG(icr, E1000_ICR, E1000_ICS)
    // Interrupt Cause Set - WO
    ADD_REG(ics, E1000_ICS, E1000_IMS)
    // Interrupt Mask Set - RW
    ADD_REG(ims, E1000_IMS, E1000_IMC)
    // Interrupt Mask Clear - WO
    ADD_REG(imc, E1000_IMC, E1000_RCTL)
    // RX Control - RW
    ADD_REG(rctl, E1000_RCTL, E1000_TCTL)
    // TX Control - RW
    ADD_REG(tctl, E1000_TCTL, E1000_TIPG)
    // TX Inter-packet gap -RW
    ADD_REG(tipg, E1000_TIPG, E1000_RDBAL)
    // RX Descriptor Base Address Low - RW
    ADD_REG(rdbal, E1000_RDBAL, E1000_RDBAH)
    // RX Descriptor Base Address High - RW
    ADD_REG(rdbah, E1000_RDBAH, E1000_RDLEN)
    // RX Descriptor Length - RW
    ADD_REG(rdlen, E1000_RDLEN, E1000_RDH)
    // RX Descriptor Head - RW
    ADD_REG(rdh, E1000_RDH, E1000_RDT)
    // RX Descriptor Tail - RW
    ADD_REG(rdt, E1000_RDT, E1000_RSRPD)
    // RX Small Packet Detect - RW
    ADD_REG(rsrpd, E1000_RSRPD, E1000_TDBAL)
    // TX Descriptor Base Address Low - RW
    ADD_REG(tdbal, E1000_TDBAL, E1000_TDBAH)
    // TX Descriptor Base Address High - RW
    ADD_REG(tdbah, E1000_TDBAH, E1000_TDLEN)
    // TX Descriptor Length - RW
    ADD_REG(tdlen, E1000_TDLEN, E1000_TDH)
    // TX Descriptor Head - RW
    ADD_REG(tdh, E1000_TDH, E1000_TDT)
    // TX Descripotr Tail - RW
    ADD_REG(tdt, E1000_TDT, E1000_TIDV)
    // TX Interrupt Delay Value - RW
    ADD_REG(tidv, E1000_TIDV, E1000_MTA)
    // Multicast Table Array - RW Array
    ADD_REG(mta, E1000_MTA, E1000_RAL0)
    // Receive Address Low - RW
    ADD_REG(ral0, E1000_RAL0, E1000_RAH0)
    // Receive Address High - RW
    ADD_REG(rah0, E1000_RAH0, E1000_RAH0 + sizeof(reg_t))
} __attribute__ ((packed));

struct tx_desc
{
        uint64_t addr;
        uint16_t length;
        uint8_t cso;
        uint8_t cmd;
        uint8_t status;
        uint8_t css;
        uint16_t special;
} __attribute__ ((packed));

struct rx_desc
{
        uint64_t addr;
        uint16_t length;
        uint16_t packet_checksum;
        uint8_t status;
        uint8_t errors;
        uint16_t special;
} __attribute__ ((packed));

volatile struct e1000_regs *e1000_reg_mem;
int irq_line;

struct tx_desc tx_desc_list[TX_DESC_COUNT];
struct PageInfo *tx_pages[TX_DESC_COUNT] = {};

struct rx_desc rx_desc_list[RX_DESC_COUNT];
struct PageInfo *rx_pages[RX_DESC_COUNT];

uint16_t read_eeprom(uint8_t addr) {
    e1000_reg_mem->eerd = EERD_START | (addr << EERD_ADDR_SHIFT);
    uint32_t result = 0;
    while (!((result = e1000_reg_mem->eerd) & EERD_DONE));
    return (result >> EERD_DATA_SHIFT) & EERD_DATA_MASK;
}

void read_mac_address(uint32_t *addr_low, uint32_t *addr_high) {
    *addr_low = read_eeprom(0);
    *addr_low |= read_eeprom(1) << 16;
    *addr_high = read_eeprom(2);
}

void setup_transmission() {
    // setup transmission ring buffer
    e1000_reg_mem->tdbal = (reg_t)va2pa(kern_pgdir, tx_desc_list);
    e1000_reg_mem->tdbah = 0;
    e1000_reg_mem->tdlen = TX_DESC_COUNT * sizeof(struct tx_desc);
    e1000_reg_mem->tdh = 0;
    e1000_reg_mem->tdt = 0;

    // setup transmission settings
    e1000_reg_mem->tctl |= TCTL_EN;
    e1000_reg_mem->tctl |= TCTL_PSP;
    e1000_reg_mem->tctl |= TCTL_CT;
    e1000_reg_mem->tctl |= TCTL_COLD;

    // setup transmission IPG time
    e1000_reg_mem->tipg |= TIPG_IPGT;
    e1000_reg_mem->tipg |= TIPG_IPGR1;
    e1000_reg_mem->tipg |= TIPG_IPGR2;

    // setup transmission interrupt timer
    e1000_reg_mem->tidv = 10;

    int i;
    // mark transmission descriptors as available
    for (i=0; i < TX_DESC_COUNT; i++) {
        tx_desc_list[i].status = TX_STATUS_DD;
        tx_desc_list[i].addr = 0;
        tx_desc_list[i].cmd = 0;
    }
}

void setup_reception() {
    // setup receive MAC address
    uint32_t mac_low = 0;
    uint32_t mac_high = 0;
    read_mac_address(&mac_low, &mac_high);
    e1000_reg_mem->ral0 = mac_low;
    e1000_reg_mem->rah0 &= ~RA_HIGH_MASK;
    e1000_reg_mem->rah0 |= RA_HIGH_MASK & mac_high;
    e1000_reg_mem->rah0 |= RA_HIGH_AV;

    // setup Multicast Table Array
    e1000_reg_mem->mta = 0;

     
    // setup Interrupt Mask Set/Read to enable interrupts
    e1000_reg_mem->ims |= ICR_RXT0;

    // setup reception ring buffer
    e1000_reg_mem->rdbal = (reg_t)va2pa(kern_pgdir, rx_desc_list);
    e1000_reg_mem->rdbah = 0;
    e1000_reg_mem->rdlen = RX_DESC_COUNT * sizeof(struct rx_desc);
    e1000_reg_mem->rdh = 0;
    e1000_reg_mem->rdt = RX_DESC_COUNT - 1;

    // preallocate pages for storing reception data
    int i;
    for (i = 0; i < RX_DESC_COUNT; i++) {
        rx_pages[i] = page_alloc(ALLOC_ZERO);
        if (rx_pages[i] == NULL) {
            panic("unable to allocate pages for network reception");
        }
        rx_pages[i]->pp_ref += 1;
    }

    // initialize reception descriptors
    for (i=0; i < RX_DESC_COUNT; i++) {
        //write data after a place for length as in pkt DS
        rx_desc_list[i].addr = (uint64_t)((page2pa(rx_pages[i]))+sizeof(int));
        rx_desc_list[i].length = 0;
        rx_desc_list[i].status = 0;
        rx_desc_list[i].errors = 0;
    }

    // setup reception settings
    e1000_reg_mem->rctl |= RCTL_EN;
    e1000_reg_mem->rctl |= RCTL_BAM;
    // strip ethernet CRC
    e1000_reg_mem->rctl |= RCTL_SECRC;
}

// LAB 6: Your driver code here
int e1000_attach(struct pci_func *pcif) {
    pci_func_enable(pcif);

    // map network card registores to memory
    e1000_reg_mem = mmio_map_region(pcif->reg_base[0], pcif->reg_size[0]);

    setup_transmission();
    setup_reception();

    // setup interrupts
    irq_setmask_8259A(irq_mask_8259A & ~(1 << pcif->irq_line));
    int i = e1000_reg_mem->icr;
    e1000_reg_mem->ims |= INT_TXDW;

    irq_line = pcif->irq_line;

    return true;
}

// takes an address to the packet data, and transmits it over the network.
// returns 0 on success, -E__NO_MEM if the transmit queue is full.
int transmit_packet(void *addr, size_t length, bool isEOP) {
    size_t cur_index = e1000_reg_mem->tdt;
    struct tx_desc *tail = &tx_desc_list[cur_index];
    if (tail->status & TX_STATUS_DD) {

        // replace existing page in the current slot with the new one
        if (tx_pages[cur_index] != NULL) {
            // ensure the page gets recycpled if every env unmapped it
            page_decref(tx_pages[cur_index]);
        }
        tx_pages[cur_index] = page_lookup(curenv->env_pgdir, addr, NULL);
        // ensure page doesn't get recycled when unmapped in userspace
        tx_pages[cur_index]->pp_ref += 1;

        // read the packet starting from the correct offset into the page
        size_t offset = addr - ROUNDDOWN(addr, PGSIZE);

        tail->cmd |= TX_CMD_RS;
        if (isEOP){
            tail->cmd |= TX_CMD_EOP;
        }
        tail->cmd |= TX_CMD_IDE;
        tail->status = 0;
        tail->addr = (uint64_t)(page2pa(tx_pages[cur_index]) + offset);
        tail->length = (uint16_t)length;
        e1000_reg_mem->tdt = (cur_index + 1) % TX_DESC_COUNT;
        return 0;
    } else {
        curenv->env_waits_for_output = true;
        curenv->env_status = ENV_WAITING_FOR_IO;
        return -E_RX_FULL;
    }
}

// takes an address to copy the received data to.
// receives over the network the next packet and copies it to the addr.
// updates pkt_size to the size received if pkt_size != NULL.
// returns 0 on success, -E_RX_EMPTY if there is no packet is available.
// returns -E_NO_MEM on allocation failure
int receive_packet(void *addr) {
    int r;
    size_t cur_index = (e1000_reg_mem->rdt + 1) % RX_DESC_COUNT;
    struct rx_desc *tail = &rx_desc_list[cur_index];

    if (!(tail->status & RX_STATUS_DD)) {
        // no packets to receive
        // env_status will be changed by an interrupt upon recv
        curenv->env_waits_for_input = true;
        curenv->env_status = ENV_WAITING_FOR_IO;
        return -E_RX_EMPTY;
    }

    // there is a packet to receive
    // write length to the beggining of page
    int *pkt_size = (int *)page2kva(rx_pages[cur_index]);
    *pkt_size = (int)tail->length;

    // allocate new page instead the one received
    struct PageInfo *replacement_page = page_alloc(ALLOC_ZERO);
    if (replacement_page == NULL) {
        return -E_NO_MEM;
    }

    //map physical page to user space at supplied addr
    //TODO: maybe write also?
    if ((r = page_insert(curenv->env_pgdir, rx_pages[cur_index], addr,
                         PTE_U | PTE_P) < 0)) {
        return -E_NO_MEM;
    }

    // decrease ref so when user unmaps it, page is recycled
    page_decref(rx_pages[cur_index]);

    rx_pages[cur_index] = replacement_page;

    // update new address
    tail->addr = (uint64_t)((page2pa(rx_pages[cur_index])) + sizeof(int));
    rx_pages[cur_index]->pp_ref += 1;
    tail->status &= ~RX_STATUS_DD;
    e1000_reg_mem->rdt = cur_index;
    return 0;
}

// handles a trap originatng from the e1000 network card
// ignores other types of traps
// returns true if the trap was handled
bool e1000_handler(int trapno) {
    int i;
    if (trapno != IRQ_OFFSET + irq_line) {
        return false;
    }

    reg_t cause = e1000_reg_mem->icr;

    if (cause & ICR_RXT0){
        for (i = 0; i < NENV; i++) {
            struct Env *env = &envs[i];
            if (env->env_status == ENV_WAITING_FOR_IO && env->env_waits_for_input) {
                env->env_waits_for_input = false;
                env->env_status = ENV_RUNNABLE;
            }
        }
    }

    else if (cause & INT_TXDW){
        for (i = 0; i < NENV; i++) {
            struct Env *env = &envs[i];
            if (env->env_status == ENV_WAITING_FOR_IO && env->env_waits_for_output) {
                env->env_waits_for_output = false;
                env->env_status = ENV_RUNNABLE;
            }
        }
    }
    
    return true;
}