#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "file.h"
#include "stat.h"   // za struct snap_info (koristi je snap_status)

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
	struct cpu *c;

	// Map "logical" addresses to virtual addresses using identity map.
	// Cannot share a CODE descriptor for both kernel and user
	// because it would have to have DPL_USR, but the CPU forbids
	// an interrupt from CPL=0 to DPL=3.
	c = &cpus[cpuid()];
	c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
	c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
	c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
	c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
	lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
	pde_t *pde;
	pte_t *pgtab;

	pde = &pgdir[PDX(va)];
	if(*pde & PTE_P){
		pgtab = (pte_t*)P2V(PTE_ADDR(*pde));
	} else {
		if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
			return 0;
		// Make sure all those PTE_P bits are zero.
		memset(pgtab, 0, PGSIZE);
		// The permissions here are overly generous, but they can
		// be further restricted by the permissions in the page table
		// entries, if necessary.
		*pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
	}
	return &pgtab[PTX(va)];
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
	char *a, *last;
	pte_t *pte;

	a = (char*)PGROUNDDOWN((uint)va);
	last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
	for(;;){
		if((pte = walkpgdir(pgdir, a, 1)) == 0)
			return -1;
		if(*pte & PTE_P)
			panic("remap");
		*pte = pa | perm | PTE_P;
		if(a == last)
			break;
		a += PGSIZE;
		pa += PGSIZE;
	}
	return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
	void *virt;
	uint phys_start;
	uint phys_end;
	int perm;
} kmap[] = {
	{ (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
	{ (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
	{ (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
	{ (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
	pde_t *pgdir;
	struct kmap *k;

	if((pgdir = (pde_t*)kalloc()) == 0)
		return 0;
	memset(pgdir, 0, PGSIZE);
	if (P2V(PHYSTOP) > (void*)DEVSPACE)
		panic("PHYSTOP too high");
	for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
		if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
		            (uint)k->phys_start, k->perm) < 0) {
			freevm(pgdir);
			return 0;
		}
	return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
	kpgdir = setupkvm();
	switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
	lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
	if(p == 0)
		panic("switchuvm: no process");
	if(p->kstack == 0)
		panic("switchuvm: no kstack");
	if(p->pgdir == 0)
		panic("switchuvm: no pgdir");

	pushcli();
	mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
		sizeof(mycpu()->ts)-1, 0);
	SEG_CLS(mycpu()->gdt[SEG_TSS]);
	mycpu()->ts.ss0 = SEG_KDATA << 3;
	mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
	// setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
	// forbids I/O instructions (e.g., inb and outb) from user space
	mycpu()->ts.iomb = (ushort) 0xFFFF;
	ltr(SEG_TSS << 3);
	lcr3(V2P(p->pgdir));  // switch to process's address space
	popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
	char *mem;

	if(sz >= PGSIZE)
		panic("inituvm: more than a page");
	mem = kalloc();
	memset(mem, 0, PGSIZE);
	mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
	memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
	uint i, pa, n;
	pte_t *pte;

	if((uint) addr % PGSIZE != 0)
		panic("loaduvm: addr must be page aligned");
	for(i = 0; i < sz; i += PGSIZE){
		if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
			panic("loaduvm: address should exist");
		pa = PTE_ADDR(*pte);
		if(sz - i < PGSIZE)
			n = sz - i;
		else
			n = PGSIZE;
		if(readi(ip, P2V(pa), offset+i, n) != n)
			return -1;
	}
	return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
	char *mem;
	uint a;

	if(newsz >= KERNBASE)
		return 0;
	if(newsz < oldsz)
		return oldsz;

	a = PGROUNDUP(oldsz);
	for(; a < newsz; a += PGSIZE){
		mem = kalloc();
		if(mem == 0){
			cprintf("allocuvm out of memory\n");
			deallocuvm(pgdir, newsz, oldsz);
			return 0;
		}
		memset(mem, 0, PGSIZE);
		if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
			cprintf("allocuvm out of memory (2)\n");
			deallocuvm(pgdir, newsz, oldsz);
			kfree(mem);
			return 0;
		}
	}
	return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
	pte_t *pte;
	uint a, pa;

	if(newsz >= oldsz)
		return oldsz;

	a = PGROUNDUP(newsz);
	for(; a  < oldsz; a += PGSIZE){
		pte = walkpgdir(pgdir, (char*)a, 0);
		if(!pte)
			a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
		else if((*pte & PTE_P) != 0){
			pa = PTE_ADDR(*pte);
			if(pa == 0)
				panic("kfree");
			char *v = P2V(pa);
			kfree(v);
			*pte = 0;
		}
	}
	return newsz;
}

// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
	uint i;

	if(pgdir == 0)
		panic("freevm: no pgdir");
	deallocuvm(pgdir, KERNBASE, 0);
	for(i = 0; i < NPDENTRIES; i++){
		if(pgdir[i] & PTE_P){
			char * v = P2V(PTE_ADDR(pgdir[i]));
			kfree(v);
		}
	}
	kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
	pte_t *pte;

	pte = walkpgdir(pgdir, uva, 0);
	if(pte == 0)
		panic("clearpteu");
	*pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
	pde_t *d;
	pte_t *pte;
	uint pa, i, flags;
	char *mem;

	if((d = setupkvm()) == 0)
		return 0;
	for(i = 0; i < sz; i += PGSIZE){
		if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
			panic("copyuvm: pte should exist");
		if(!(*pte & PTE_P))
			panic("copyuvm: page not present");
		pa = PTE_ADDR(*pte);
		flags = PTE_FLAGS(*pte);
		if((mem = kalloc()) == 0)
			goto bad;
		memmove(mem, (char*)P2V(pa), PGSIZE);
		if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
			kfree(mem);
			goto bad;
		}
	}
	return d;

bad:
	freevm(d);
	return 0;
}

// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
	pte_t *pte;

	pte = walkpgdir(pgdir, uva, 0);
	if((*pte & PTE_P) == 0)
		return 0;
	if((*pte & PTE_U) == 0)
		return 0;
	return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
	char *buf, *pa0;
	uint n, va0;

	buf = (char*)p;
	while(len > 0){
		va0 = (uint)PGROUNDDOWN(va);
		pa0 = uva2ka(pgdir, (char*)va0);
		if(pa0 == 0)
			return -1;
		n = PGSIZE - (va - va0);
		if(n > len)
			n = len;
		memmove(pa0 + (va - va0), buf, n);
		len -= n;
		buf += n;
		va = va0 + PGSIZE;
	}
	return 0;
}

// Snimak = nezavisna kopija korisnicke memorije: sopstvena tabela stranica
// (pgdir) + sopstvene fizicke stranice. Proces ima BROJ_SLOTOVA slotova;
// slot je prazan kad mu je pgdir == 0.
//
// Vecinu posla rade postojece funkcije iznad:
//   copyuvm(pgdir, sz) - napravi novu tabelu + nove stranice + kopira sadrzaj;
//                        pri gresci sam oslobodi delimicno alocirano (rollback).
//   freevm(pgdir)      - oslobodi tabelu i sve njene fizicke stranice.
// Zato se snap_take/restore svode na copyuvm, a snap_release/exit na freevm.
//
// Sve snap_* rade nad myproc(); pomocne (init/kopiraj/oslobodi) nad datim p.

// Da li je slot u opsegu [0, BROJ_SLOTOVA)? Proveri pre indeksiranja
// p->snimci[slot], da se ne cita van granica niza.
static int
validan_slot(int slot)
{
	return slot >= 0 && slot < BROJ_SLOTOVA;
}

// Broj fizickih stranica koje snimak velicine sz zauzima. Korisnicka memorija
// je neprekidna od 0 do sz, pa je dovoljno zaokruziti sz na celu stranicu.
static int
broj_stranica(uint sz)
{
	return PGROUNDUP(sz) / PGSIZE;
}

// snap_take: napravi snimak trenutne korisnicke memorije i smesti ga u slot.
//
//   0  uspeh
//  -1  neuspela alokacija (nema dovoljno memorije)
//  -2  nevalidan broj slota (van 0..3)
//  -3  slot vec sadrzi snimak
//  -4  ostale greske
int
snap_take(int slot)
{
	struct proc *p = myproc();
	pde_t *kopija;            // tabela stranica novog snimka

	if(!validan_slot(slot))
		return -2;            // nevalidan broj slota
	if(p->snimci[slot].pgdir != 0)
		return -3;            // slot je vec zauzet

	// copyuvm pravi novu tabelu, alocira nove stranice i kopira sadrzaj na
	// iste virtuelne adrese; pri gresci sam oslobodi sve i vrati 0 (rollback).
	kopija = copyuvm(p->pgdir, p->sz);
	if(kopija == 0)
		return -1;            // nije bilo dovoljno memorije

	// Kopiju upisujemo u slot tek kad je sigurno uspela.
	p->snimci[slot].pgdir = kopija;
	p->snimci[slot].sz = p->sz;
	return 0;
}

// snap_restore: vrati memoriju procesa na stanje sacuvano u slotu.
// Snimak u slotu ne smemo unistiti (treba da ostane za visestruka vracanja),
// pa pravimo svezu kopiju snimka i njom zamenjujemo trenutnu memoriju.
//
//   0  uspeh
//  -1  slot je prazan
//  -2  nevalidan broj slota (van 0..3)
//  -3  ostale greske
int
snap_restore(int slot)
{
	struct proc *p = myproc();
	pde_t *nova;             // sveza kopija snimka
	pde_t *stara;            // trenutna tabela koju cemo osloboditi

	if(!validan_slot(slot))
		return -2;            // nevalidan broj slota
	if(p->snimci[slot].pgdir == 0)
		return -1;            // slot je prazan

	// Prvo napravimo kopiju snimka. Tek ako uspe, rusimo staru memoriju -
	// tako proces ostaje netaknut ako kopiranje ne uspe (nema memorije).
	nova = copyuvm(p->snimci[slot].pgdir, p->snimci[slot].sz);
	if(nova == 0)
		return -3;            // nema memorije za svezu kopiju snimka

	stara = p->pgdir;         // sacuvamo staru da je oslobodimo NA KRAJU
	p->pgdir = nova;
	p->sz = p->snimci[slot].sz;

	// Ucitaj novu tabelu u CR3 pre oslobadjanja stare; obrnut redosled bi
	// oslobodio tabelu na koju procesor jos pokazuje. switchuvm radi lcr3,
	// cime se osvezava i TLB.
	switchuvm(p);
	freevm(stara);
	return 0;
}

// snap_diff: vrati broj stranica koje se razlikuju izmedju dva snimka.
// Prolazimo kroz obe tabele stranica, nalazimo fizicke adrese i poredimo
// sadrzaj stranicu po stranicu, bajt po bajt. I jedan razlicit bajt znaci
// da je cela stranica "razlicita". Ako snimci imaju razlicit broj stranica,
// svaka "visak" stranica veceg snimka se takodje broji kao razlika.
//
//  >=0  broj stranica koje se razlikuju
//   -1  jedan od slotova je prazan
//   -2  nevalidan broj slota (van 0..3)
//   -3  ostale greske
int
snap_diff(int slot1, int slot2)
{
	struct proc *p = myproc();
	pde_t *tabela1, *tabela2;        // tabele stranica dva snimka
	uint kraj;              // dokle idemo (unija oba snimka, po stranicama)
	uint va;
	int razlike = 0;         // broj razlicitih stranica

	if(!validan_slot(slot1) || !validan_slot(slot2))
		return -2;            // nevalidan broj slota
	if(p->snimci[slot1].pgdir == 0 || p->snimci[slot2].pgdir == 0)
		return -1;            // bar jedan slot je prazan

	tabela1 = p->snimci[slot1].pgdir;
	tabela2 = p->snimci[slot2].pgdir;

	// Idemo do vece od dve velicine - tako pokrivamo i "visak" stranice.
	kraj = PGROUNDUP(p->snimci[slot1].sz);
	if(PGROUNDUP(p->snimci[slot2].sz) > kraj)
		kraj = PGROUNDUP(p->snimci[slot2].sz);

	for(va = 0; va < kraj; va += PGSIZE){
		pte_t *pte1 = walkpgdir(tabela1, (void*)va, 0);
		pte_t *pte2 = walkpgdir(tabela2, (void*)va, 0);
		int ima1 = pte1 && (*pte1 & PTE_P);   // postoji li stranica u snimku 1
		int ima2 = pte2 && (*pte2 & PTE_P);   // postoji li stranica u snimku 2

		if(ima1 && ima2){
			// Obe stranice postoje -> uporedi sadrzaj bajt po bajt.
			// PTE_ADDR daje fizicku adresu, P2V je prevodi u adresu koju
			// kernel moze direktno da cita iz svog (mapiranog) prostora.
			char *a = (char*)P2V(PTE_ADDR(*pte1));
			char *b = (char*)P2V(PTE_ADDR(*pte2));
			if(memcmp(a, b, PGSIZE) != 0)
				razlike++;
		} else if(ima1 || ima2){
			// Stranica postoji samo u jednom snimku => razlika u velicini.
			razlike++;
		}
	}
	return razlike;
}

// snap_release: oslobodi resurse koje zauzima snimak u slotu.
//
//   0  uspeh
//  -1  slot je vec bio prazan
//  -2  nevalidan broj slota (van 0..3)
//  -3  ostale greske
int
snap_release(int slot)
{
	struct proc *p = myproc();

	if(!validan_slot(slot))
		return -2;            // nevalidan broj slota
	if(p->snimci[slot].pgdir == 0)
		return -1;            // slot je vec bio prazan

	// freevm oslobadja i tabelu stranica i sve fizicke stranice snimka.
	freevm(p->snimci[slot].pgdir);
	p->snimci[slot].pgdir = 0;   // oznaci slot kao prazan
	p->snimci[slot].sz = 0;
	return 0;
}

// snap_status: popuni korisnicku strukturu podacima o snimku
// (velicina u bajtovima i broj fizickih stranica).
//
//   0  uspeh
//  -1  slot je prazan
//  -2  ostale greske (ovde i: nevalidan broj slota)
int
snap_status(int slot, struct snap_info *si)
{
	struct proc *p = myproc();

	if(!validan_slot(slot))
		return -2;            // nevalidan slot spada u "ostale greske"
	if(p->snimci[slot].pgdir == 0)
		return -1;            // slot je prazan

	// 'si' je korisnicki pokazivac, ali posto radimo nad TRENUTNIM procesom
	// cija je tabela stranica aktivna, kernel sme da pise direktno preko
	// njega (isti princip kao sys_fstat). Validnost pokazivaca je vec
	// proverena u sys_snap_status preko argptr.
	si->size = p->snimci[slot].sz;
	si->num_pages = broj_stranica(p->snimci[slot].sz);
	return 0;
}

// snap_empty: vrati indeks prvog praznog (slobodnog) slota.
//
//  >=0  indeks prvog praznog slota
//   -1  nema praznog slota
//   -2  ostale greske
int
snap_empty(void)
{
	struct proc *p = myproc();
	int i;

	for(i = 0; i < BROJ_SLOTOVA; i++)
		if(p->snimci[i].pgdir == 0)
			return i;         // prvi prazan slot
	return -1;                // svi slotovi su zauzeti
}

//  Pomocne funkcije za integraciju sa fork() / exit() / allocproc()

// init_snimaka: oznaci sve slotove procesa kao prazne.
// Zove se iz allocproc() pri kreiranju procesa - garantuje da nov proces
// (ili recikliran slot u tabeli procesa) krece sa cistim, praznim slotovima.
void
init_snimaka(struct proc *p)
{
	int i;

	for(i = 0; i < BROJ_SLOTOVA; i++){
		p->snimci[i].pgdir = 0;
		p->snimci[i].sz = 0;
	}
}

// kopiraj_snimke: prekopiraj sve popunjene slotove roditelja u dete (svaki kao
// nezavisnu kopiju). Zove se iz fork(). Vraca 0, ili -1 uz oslobadjanje vec
// kopiranih slotova.
int
kopiraj_snimke(struct proc *parent, struct proc *child)
{
	int i, j;
	pde_t *kopija;

	for(i = 0; i < BROJ_SLOTOVA; i++){
		if(parent->snimci[i].pgdir == 0)
			continue;          // prazan slot se preskace

		kopija = copyuvm(parent->snimci[i].pgdir, parent->snimci[i].sz);
		if(kopija == 0){
			// Nema memorije: ROLLBACK - oslobodi sve sto smo u dete vec
			// kopirali (slotove 0..i-1), pa javi gresku.
			for(j = 0; j < i; j++){
				if(child->snimci[j].pgdir){
					freevm(child->snimci[j].pgdir);
					child->snimci[j].pgdir = 0;
					child->snimci[j].sz = 0;
				}
			}
			return -1;
		}
		child->snimci[i].pgdir = kopija;
		child->snimci[i].sz = parent->snimci[i].sz;
	}
	return 0;
}

// oslobodi_snimke: oslobodi sve snimke procesa. Zove se iz exit() da bi se
// sprecilo curenje memorije kada proces zavrsi rad.
void
oslobodi_snimke(struct proc *p)
{
	int i;

	for(i = 0; i < BROJ_SLOTOVA; i++){
		if(p->snimci[i].pgdir){
			freevm(p->snimci[i].pgdir);
			p->snimci[i].pgdir = 0;
			p->snimci[i].sz = 0;
		}
	}
}
