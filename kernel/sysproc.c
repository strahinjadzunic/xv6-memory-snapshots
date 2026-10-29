#include "types.h"
#include "defs.h"
#include "proc.h"
#include "stat.h"   // za struct snap_info (koristi je sys_snap_status)

int
sys_fork(void)
{
	return fork();
}

int
sys_exit(void)
{
	exit();
	return 0;  // not reached
}

int
sys_wait(void)
{
	return wait();
}

int
sys_kill(void)
{
	int pid;

	if(argint(0, &pid) < 0)
		return -1;
	return kill(pid);
}

int
sys_getpid(void)
{
	return myproc()->pid;
}

int
sys_sbrk(void)
{
	int addr;
	int n;

	if(argint(0, &n) < 0)
		return -1;
	addr = myproc()->sz;
	if(growproc(n) < 0)
		return -1;
	return addr;
}

int
sys_sleep(void)
{
	int n;
	uint ticks0;

	if(argint(0, &n) < 0)
		return -1;
	acquire(&tickslock);
	ticks0 = ticks;
	while(ticks - ticks0 < n){
		if(myproc()->killed){
			release(&tickslock);
			return -1;
		}
		sleep(&ticks, &tickslock);
	}
	release(&tickslock);
	return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
	uint xticks;

	acquire(&tickslock);
	xticks = ticks;
	release(&tickslock);
	return xticks;
}

//  Snapshot sistemski pozivi - "omotaci" (wrappers)
//
//  Posao omotaca je da procita argumente iz korisnickog prostora (argint za
//  int-ove, argptr za pokazivace) i pozove pravu logiku iz vm.c. Sistemski
//  pozivi ne ispisuju nista - vracaju samo povratne vrednosti, a ispis radi
//  korisnicki program.
//
//  Ako citanje argumenta ne uspe, vraca se "ostala greska" tog poziva
//  (npr. -4 za snap_take, -3 za restore/diff/release, -2 za status).

int
sys_snap_take(void)
{
	int slot;

	if(argint(0, &slot) < 0)
		return -4;            // nije uspelo citanje argumenta -> ostale greske
	return snap_take(slot);
}

int
sys_snap_restore(void)
{
	int slot;

	if(argint(0, &slot) < 0)
		return -3;
	return snap_restore(slot);
}

int
sys_snap_diff(void)
{
	int slot1, slot2;

	if(argint(0, &slot1) < 0 || argint(1, &slot2) < 0)
		return -3;
	return snap_diff(slot1, slot2);
}

int
sys_snap_release(void)
{
	int slot;

	if(argint(0, &slot) < 0)
		return -3;
	return snap_release(slot);
}

int
sys_snap_status(void)
{
	int slot;
	struct snap_info *si;

	if(argint(0, &slot) < 0)
		return -2;
	// argptr proveri da pokazivac (i ceo struct na njega) lezi u adresnom
	// prostoru korisnika - tako se branimo od losih/zlonamernih pokazivaca.
	if(argptr(1, (void*)&si, sizeof(*si)) < 0)
		return -2;
	return snap_status(slot, si);
}

int
sys_snap_empty(void)
{
	return snap_empty();
}
