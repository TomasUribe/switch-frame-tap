#ifndef TIER4_PROBES_H
#define TIER4_PROBES_H

void probe_sys(void);     /* firmware, model, ticks - run once  */
void probe_psm(void);     /* charger type + battery - each pass */
void probe_caps(int pass);/* caps:sc capture paths  - each pass */
void probe_mmio(void);    /* svcQueryIoMapping + DC dump - once */
void probe_nv(void);      /* which /dev/nv* nodes open   - once */
void probe_apm(void);     /* performance/operation mode  - once */

#endif
