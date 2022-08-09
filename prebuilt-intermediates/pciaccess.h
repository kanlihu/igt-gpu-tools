#pragma once
#include<inttypes.h>

#define PCI_MATCH_ANY  (~0)

/**
 * Compare two PCI ID values (either vendor or device).  This is used
 * internally to compare the fields of \c pci_id_match to the fields of
 * \c pci_device.
 */
#define PCI_ID_COMPARE(a, b) \
    (((a) == PCI_MATCH_ANY) || ((a) == (b)))

typedef uint64_t pciaddr_t;
struct pci_mem_region {
    void *memory;
    pciaddr_t bus_addr;
    pciaddr_t base_addr;
    pciaddr_t size;
    unsigned is_IO:1;
    unsigned is_prefetchable:1;
    unsigned is_64:1;
};
struct pci_device {
    uint16_t domain;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subvendor_id;
    uint16_t subdevice_id;
    uint32_t device_class;
    uint8_t revision;
    struct pci_mem_region regions[6];
    pciaddr_t rom_size;
    int irq;
    intptr_t user_data;
    int vgaarb_rsrc;
};
struct pci_id_match {
    /**
     * \name Device / vendor matching controls
     *
     * Control the search based on the device, vendor, subdevice, or subvendor
     * IDs.  Setting any of these fields to \c PCI_MATCH_ANY will cause the
     * field to not be used in the comparison.
     */
    /*@{*/
    uint32_t    vendor_id;
    uint32_t    device_id;
    uint32_t    subvendor_id;
    uint32_t    subdevice_id;
    /*@}*/


    /**
     * \name Device class matching controls
     *
     */
    /*@{*/
    uint32_t    device_class;
    uint32_t    device_class_mask;
    /*@}*/

    intptr_t    match_data;
};
