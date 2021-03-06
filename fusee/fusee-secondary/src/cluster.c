#include <stdint.h>
 
#include "cluster.h"
#include "flow.h"
#include "sysreg.h"
#include "i2c.h"
#include "car.h"
#include "timers.h"
#include "pmc.h"
#include "max77620.h"

void _cluster_enable_power()
{
    uint8_t val = 0;
    i2c_query(4, 0x3C, MAX77620_REG_AME_GPIO, &val, 1);
    
    val &= 0xDF;
    i2c_send(4, 0x3C, MAX77620_REG_AME_GPIO, &val, 1);
    val = 0x09;
    i2c_send(4, 0x3C, MAX77620_REG_GPIO5, &val, 1);
    
    /* Enable power. */
    val = 0x20;
    i2c_send(4, 0x1B, MAX77620_REG_CNFGGLBL3, &val, 1);
    val = 0x8D;
    i2c_send(4, 0x1B, MAX77620_REG_CNFG1_32K, &val, 1);
    val = 0xB7;
    i2c_send(4, 0x1B, MAX77620_REG_CNFGGLBL1, &val, 1);
    val = 0xB7;
    i2c_send(4, 0x1B, MAX77620_REG_CNFGGLBL2, &val, 1);
}

int _cluster_pmc_enable_partition(uint32_t part, uint32_t toggle)
{
    volatile tegra_pmc_t *pmc = pmc_get_regs();
    
    /* Check if the partition has already been turned on. */
    if (pmc->pwrgate_status & part)
        return 1;

    uint32_t i = 5001;
    while (pmc->pwrgate_toggle & 0x100)
    {
        udelay(1);
        i--;
        if (i < 1)
            return 0;
    }

    pmc->pwrgate_toggle = (toggle | 0x100);

    i = 5001;
    while (i > 0)
    {
        if (pmc->pwrgate_status & part)
            break;
        
        udelay(1);
        i--;
    }

    return 1;
}

void cluster_boot_cpu0(uint32_t entry)
{
    volatile tegra_car_t *car = car_get_regs();
    
    /* Set ACTIVE_CLUSER to FAST. */
    FLOW_CTLR_BPMP_CLUSTER_CONTROL_0 &= 0xFFFFFFFE;

    _cluster_enable_power();
    
    if (!(car->pllx_base & 0x40000000))
    {
        car->pllx_misc3 &= 0xFFFFFFF7;
        udelay(2);
        car->pllx_base = 0x80404E02;
        car->pllx_base = 0x404E02;
        car->pllx_misc = ((car->pllx_base & 0xFFFBFFFF) | 0x40000);
        car->pllx_base = 0x40404E02;
    }
    
    while (!(car->pllx_base & 0x8000000)) {
        /* Spinlock. */
    }

    /* Configure MSELECT source and enable clock. */
    car->clk_source_mselect = ((car->clk_source_mselect & 0x1FFFFF00) | 6);
    car->clk_out_enb_v = ((car->clk_out_enb_v & 0xFFFFFFF7) | 8);

    /* Configure initial CPU clock frequency and enable clock. */
    car->cclk_brst_pol = 0x20008888;
    car->super_cclk_div = 0x80000000;
    car->clk_enb_v_set = 1;
    
    clkrst_reboot(CARDEVICE_CORESIGHT);

    /* CAR2PMC_CPU_ACK_WIDTH should be set to 0. */
    car->cpu_softrst_ctrl2 &= 0xFFFFF000;

    /* Enable CPU rail. */
    _cluster_pmc_enable_partition(1, 0);
    
    /* Enable cluster 0 non-CPU. */
    _cluster_pmc_enable_partition(0x8000, 15);
    
    /* Enable CE0. */
    _cluster_pmc_enable_partition(0x4000, 14);

    /* Request and wait for RAM repair. */
    FLOW_CTLR_RAM_REPAIR_0 = 1;
    while (!(FLOW_CTLR_RAM_REPAIR_0 & 2)){
        /* Spinlock. */
    }

    MAKE_EXCP_VEC_REG(0x100) = 0;

    /* Set reset vector. */
    SB_AA64_RESET_LOW_0 = entry | 1;
    SB_AA64_RESET_HIGH_0 = 0;
    
    /* Non-secure reset vector write disable. */
    SB_CSR_0 = 2;
    (void)SB_CSR_0;

    /* Clear MSELECT reset. */
    car->rst_dev_v &= 0xFFFFFFF7;
    
    /* Clear NONCPU reset. */
    car->rst_cpug_cmplx_clr = 0x20000000;
    
    /* Clear CPU{0,1,2,3} POR and CORE, CX0, L2, and DBG reset.*/
    car->rst_cpug_cmplx_clr = 0x411F000F;
}
