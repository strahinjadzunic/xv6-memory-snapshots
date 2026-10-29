// snaptest - korisnicki program za testiranje snapshot sistemskih poziva.
//
// Program se ne ocenjuje; sluzi za brzu proveru ponasanja. Pokriva i obicne
// putanje i rubne slucajeve gresaka (nevalidan slot, prazan slot, zauzet slot,
// svi slotovi puni, los pokazivac, ponovna upotreba slota...).
// Slobodno ga prilagodi/dopuni.
//
// Napomena o semantici snap_restore:
// snap_restore vraca celokupnu korisnicku memoriju procesa, ukljucujuci i
// stek. Registri (pa ni programski brojac) se ne vracaju. Posto su povratne
// adrese funkcija na steku, posle uspesnog snap_restore izvrsavanje se "vrati"
// na tacku odmah posle onog snap_take koji je napravio snimak (kao da se
// snap_take tek vratio). Zato bi naivni test oblika
//     snap_take(0); x = 99; snap_restore(0);
// vrteo u beskonacnoj petlji.
//
// Trik: stanje slotova zivi u kernelu (struct proc), a ne u korisnickoj
// memoriji, pa ga snap_restore ne vraca. "Marker" snimak u drugom slotu nam
// sluzi da prepoznamo da smo se vec vratili iz restore-a i prekinemo petlju.

#include "kernel/stat.h"   // struct snap_info
#include "user.h"

// Globalne promenljive koje cemo snimati i posmatrati (data segment).
int brojac = 0;
char veliki[8192];              // 2 stranice podataka

// Mali pomocnik za ispis rezultata jednog testa.
static void
proveri(char *opis, int uslov)
{
	printf("[%s] %s\n", uslov ? "OK " : "GRESKA", opis);
}

// Popuni globalni bafer nekom vrednoscu (da bismo imali sta da menjamo).
static void
popuni(char vrednost)
{
	int i;
	for(i = 0; i < (int)sizeof(veliki); i++)
		veliki[i] = vrednost;
}

// Oslobodi sve slotove (da svaki test krene iz poznatog stanja).
static void
oslobodi_sve(void)
{
	int i;
	for(i = 0; i < 4; i++)
		snap_release(i);   // -1 ako je vec prazan; nije bitno ovde
}

// Demonstracija uspesnog snap_restore u zasebnom procesu (vidi objasnjenje gore).
// Pokazuje i da snap_restore ne unistava snimak u slotu (uslov za visestruka
// vracanja iz iste tacke).
static void
test_restore_u_detetu(void)
{
	struct snap_info si;
	int pid = fork();

	if(pid == 0){
		brojac = 7;
		snap_take(0);                 // <-- tacka na koju se vracamo posle restore

		if(snap_status(1, &si) == 0){
			// Slot 1 je pun => marker je "preziveo" restore => vec smo se vratili.
			// (Posle uspesnog restore izvrsavanje se nastavlja sa vracenim
			// stekom; zato ovde radimo samo proste %d ispise, ne slozene izraze.)
			printf("[dete] posle snap_restore: brojac = %d (ocekivano 7)\n",
			       brojac);
			// Slot 0 mora i dalje biti pun: snap_restore ne unistava snimak.
			printf("[dete] slot 0 posle restore: status = %d "
			       "(ocekivano 0 => snimak ocuvan, moguca visestruka vracanja)\n",
			       snap_status(0, &si));
			exit();
		}

		// Prvi prolaz: postavi marker u slot 1, promeni memoriju, pa restore.
		snap_take(1);                 // marker (kernel stanje - prezivljava restore)
		brojac = 99;                  // promena posle snimka
		snap_restore(0);              // vraca memoriju -> skace nazad iznad snap_take(0)
		proveri("[dete] ovde se ne sme stici", 0);   // normalno nedostizno
		exit();
	}
	wait();
}

int
main(void)
{
	struct snap_info si;
	int r;

	printf("\n=== snaptest: pocetak ===\n");

	// 1) Pocetno stanje
	proveri("snap_empty na pocetku vraca 0 (prvi prazan slot)",
	        snap_empty() == 0);

	// 2) snap_take + snap_status (validni slotovi, ukljucujuci granice 0 i 3)
	proveri("snap_take(0) uspeva [donja granica slota]", snap_take(0) == 0);

	r = snap_status(0, &si);
	proveri("snap_status(0) uspeva", r == 0);
	proveri("snap_status: num_pages = ceil(size/4096)",
	        si.num_pages == (si.size + 4095) / 4096);
	printf("    -> size=%d B, num_pages=%d\n", si.size, si.num_pages);

	proveri("snap_take(3) uspeva [gornja validna granica slota]",
	        snap_take(3) == 0);
	proveri("snap_status(3) uspeva", snap_status(3, &si) == 0);
	proveri("snap_empty sada vraca 1 (slotovi 0 i 3 zauzeti)",
	        snap_empty() == 1);
	oslobodi_sve();

	// 3) snap_take - sve greske i granice opsega
	proveri("snap_take(0) uspeva (priprema)", snap_take(0) == 0);
	proveri("snap_take(0) ponovo vraca -3 (slot zauzet)", snap_take(0) == -3);
	proveri("snap_take(4) vraca -2 [tacno preko gornje granice]",
	        snap_take(4) == -2);
	proveri("snap_take(-1) vraca -2 (negativan slot)", snap_take(-1) == -2);
	proveri("snap_take(99) vraca -2 (daleko van opsega)", snap_take(99) == -2);
	oslobodi_sve();

	// 4) snap_empty - kad su SVI slotovi puni, pa opet prazni
	snap_take(0); snap_take(1); snap_take(2); snap_take(3);
	proveri("snap_empty vraca -1 kad su svi slotovi puni", snap_empty() == -1);
	oslobodi_sve();
	proveri("snap_empty opet vraca 0 posle oslobadjanja svih", snap_empty() == 0);

	// 5) snap_diff - razlike + svi rubni slucajevi
	popuni('A');
	proveri("snap_take(0) [bafer='A'] uspeva", snap_take(0) == 0);
	popuni('B');                 // izmena bafera -> menja njegove stranice
	proveri("snap_take(1) [bafer='B'] uspeva", snap_take(1) == 0);

	r = snap_diff(0, 1);
	proveri("snap_diff(0,1) > 0 (bafer je promenjen)", r > 0);
	printf("    -> snap_diff(0,1) = %d razlicitih stranica\n", r);

	proveri("snap_diff(0,0) == 0 (isti snimak)", snap_diff(0, 0) == 0);
	proveri("snap_diff(0,2) == -1 (drugi slot prazan)", snap_diff(0, 2) == -1);
	proveri("snap_diff(2,3) == -1 (oba slota prazna)", snap_diff(2, 3) == -1);
	proveri("snap_diff(0,99) == -2 (drugi slot nevalidan)", snap_diff(0, 99) == -2);
	proveri("snap_diff(99,0) == -2 (prvi slot nevalidan)", snap_diff(99, 0) == -2);
	proveri("snap_diff(-1,0) == -2 (negativan slot)", snap_diff(-1, 0) == -2);
	oslobodi_sve();

	// 6) snap_release - greske + ponovno koriscenje slota posle release
	proveri("snap_take(2) uspeva (priprema)", snap_take(2) == 0);
	proveri("snap_release(2) uspeva", snap_release(2) == 0);
	proveri("snap_release(2) ponovo vraca -1 (vec prazan)", snap_release(2) == -1);
	proveri("snap_release(99) vraca -2 (nevalidan slot)", snap_release(99) == -2);
	proveri("snap_release(-1) vraca -2 (negativan slot)", snap_release(-1) == -2);
	proveri("snap_take(2) opet uspeva [slot se ponovo koristi posle release]",
	        snap_take(2) == 0);
	oslobodi_sve();

	// 7) snap_status - greske + los pokazivac
	proveri("snap_take(0) uspeva (priprema)", snap_take(0) == 0);
	proveri("snap_status(0) uspeva", snap_status(0, &si) == 0);
	proveri("snap_status(1) vraca -1 (slot prazan)", snap_status(1, &si) == -1);
	proveri("snap_status(99) vraca -2 (nevalidan slot)", snap_status(99, &si) == -2);
	// Los korisnicki pokazivac: argptr u sys_snap_status ga odbija -> -2.
	// (Adresa van adresnog prostora procesa; kernel je NE dereferencira.)
	proveri("snap_status(0, los_pokazivac) vraca -2 (argptr provera)",
	        snap_status(0, (struct snap_info*)0xFFFFF000) == -2);
	oslobodi_sve();

	// 8) snap_restore - greske (bezbedno u glavnom procesu)
	// Greska se vraca PRE diranja memorije, pa nema "skoka unazad".
	proveri("snap_restore(2) na prazan slot vraca -1", snap_restore(2) == -1);
	proveri("snap_restore(99) vraca -2 (nevalidan slot)", snap_restore(99) == -2);
	proveri("snap_restore(-1) vraca -2 (negativan slot)", snap_restore(-1) == -2);

	// 9) snap_restore - uspeh + dokaz da ne unistava slot (u zasebnom procesu)
	test_restore_u_detetu();

	// 10) fork - dete nasledjuje snimke kao nezavisne kopije
	oslobodi_sve();
	proveri("snap_take(0) pred fork uspeva", snap_take(0) == 0);

	int pid = fork();
	if(pid == 0){
		struct snap_info ci;
		proveri("[dete] snap_status(0) uspeva (snimak nasledjen)",
		        snap_status(0, &ci) == 0);
		// Dete oslobadja svoju kopiju - ne sme uticati na roditelja.
		proveri("[dete] snap_release(0) uspeva", snap_release(0) == 0);
		exit();
	}
	wait();
	proveri("[roditelj] slot 0 i dalje pun posle fork/release deteta "
	        "(kopije su nezavisne)", snap_status(0, &si) == 0);
	oslobodi_sve();

	// Napomena o gresakama koje se NE mogu lako izazvati iz obicnog testa:
	//   snap_take == -1  (nema slobodne fizicke memorije)  -> trazi pritisak
	//                     na memoriju (npr. sbrk do iscrpljenja pre take).
	//   snap_restore == -3, snap_take == -4 (ostale greske) -> retke putanje.
	// Na odbrani se -1 obicno demonstrira vestackim trosenjem memorije.

	printf("=== snaptest: kraj ===\n\n");
	exit();
}
