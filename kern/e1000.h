#ifndef JOS_KERN_E1000_H
#define JOS_KERN_E1000_H

#include <kern/pci.h>
#include <inc/ns.h>

#define E1000_VENDOR_ID 0x8086
#define E1000_PRODUCT_ID 0x100E

int e1000_attach(struct pci_func *pcif);
bool e1000_handler(int trapno);
int transmit_packet(void *addr, size_t length, bool isEOP);
int receive_packet(void *addr);
void read_mac_address(uint32_t *addr_low, uint32_t *addr_high);


#endif	// JOS_KERN_E1000_H
