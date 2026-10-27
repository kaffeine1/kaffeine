# Kaffeine: visione e obiettivo a lungo termine

[English version](MANIFESTO.md) | [Implementazione attuale](../../README.KAFFEINE.md)

## 1. La visione

Costruire una casa open source comune per il software dell’ecosistema Amiga, capace di evolvere con l’hardware senza perdere il patrimonio delle sue diverse piattaforme.

AROS è il sistema operativo nativo. kaffeine.library è l’infrastruttura di compatibilità che deve permettere alle applicazioni provenienti da architetture e ambienti differenti di convivere nello stesso desktop.

L’obiettivo non è creare un’altra fazione dell’Amiga né imporre l’abbandono dei sistemi esistenti. È ridurre la frammentazione dal punto di vista dell’utente: scegliere un programma per ciò che permette di fare, non in funzione della CPU o del ramo dell’ecosistema per cui è stato scritto.

ARM64 è il primo host, non il confine del progetto. Il 68k è il primo ambiente compatibile, non necessariamente l’ultimo.

Questa è una direzione di lungo periodo, non una dichiarazione di compatibilità già raggiunta. Prima di ogni intervento, verificare lo stato corrente del repository e delle prove, senza considerare automaticamente implementati gli obiettivi descritti qui.

## 2. L’esperienza che vogliamo offrire

L’utente deve poter avviare un’applicazione supportata dal desktop o dalla Shell e utilizzarla come parte dell’ambiente AROS, senza dover gestire manualmente un emulatore o una macchina virtuale separata.

La compatibilità deve comprendere progressivamente il ciclo d’uso completo: avvio, interazione, accesso ai file, caricamento delle dipendenze, salvataggio, chiusura e rilascio delle risorse. Dove possibile, deve estendersi a integrazioni come appunti, trascinamento, icone, requester e comunicazione fra applicazioni.

Il risultato desiderato è eseguire i binari originali supportati senza richiederne la ricompilazione o modifiche specifiche per kaffeine. Programmi ricompilati e fixture dedicate rimangono strumenti preziosi di sviluppo, ma non sostituiscono la verifica con software preesistente.

Personal Paint è il primo grande traguardo grafico concreto: avviarlo, arrivare all’editor, usare gli strumenti, caricare e salvare immagini, chiuderlo correttamente e infine verificarne l’avvio dal desktop e la fluidità.

Le correzioni necessarie devono implementare contratti generali. Evitare comportamenti speciali legati al nome di Personal Paint o di un altro programma, salvo eccezioni motivate, isolate e documentate.

## 3. Il patrimonio da preservare: la compatibilità, indipendentemente dalla CPU host

La parte più importante del progetto è la conoscenza verificata del comportamento delle API, degli oggetti e dei formati Amiga.

Una correzione alla gestione di una lista, di un messaggio, di un font o di una libreria deve poter essere riutilizzata passando da ARM64 a RISC-V o a un altro host. Non deve essere necessario risolvere nuovamente gli stessi problemi di compatibilità per ogni processore.

L’architettura deve mantenere separati questi livelli:

| Livello | Responsabilità |
| --- | --- |
| Ambiente di compatibilità guest | Convenzioni di chiamata, comportamento delle API, oggetti, strutture, handle e callback dell’ambiente supportato. |
| Caricamento | Eseguibili, rilocazioni, librerie, inizializzazione e convenzioni di avvio specifiche del formato e dell’ambiente guest. |
| Esecuzione CPU guest | Interpretazione o traduzione delle istruzioni, registri, flag, eccezioni e modello CPU dichiarato. |
| Servizi della piattaforma host | Memoria eseguibile, pubblicazione del codice, cache, sincronizzazione e altri servizi dipendenti da CPU e sistema operativo. |


Questa separazione non impone una riscrittura preventiva del progetto. Le astrazioni vanno introdotte dove riducono dipendenze reali e sono dimostrate da implementazioni e test.

Non trasformare oggi il sistema in un framework universale solo per anticipare esigenze ipotetiche.

## 4. Portabilità: C e C++ come strumenti, contratti espliciti come requisito

Il codice comune deve rimanere il più possibile indipendente dall’architettura host. L’uso di C e C++ aiuta, ma non rende automaticamente portabili le assunzioni su puntatori, allineamento, ordine dei byte e convenzioni di chiamata.

Gli indirizzi guest devono restare distinti dai puntatori host. I campi guest devono avere dimensioni e rappresentazioni esplicite. Gli offset delle strutture native devono essere ricavati o verificati per il target, senza presumere che un layout ottenuto su x86_64 sia valido ovunque.

La conversione dei dati deve dipendere dal loro significato. Un valore numerico non deve diventare un puntatore soltanto perché rientra nell’intervallo degli indirizzi guest.

Il generatore JIT AArch64 rimane specifico di AArch64 anche se è scritto in C. Un futuro host RISC-V richiederà un proprio backend di generazione e i relativi servizi di esecuzione. Il ponte di compatibilità, i loader e la parte riutilizzabile dei test devono invece poter essere conservati.

L’obiettivo del futuro port è quindi:
Cambiare ciò che dipende dall’host, preservando ciò che abbiamo imparato e verificato sul software Amiga.

## 5. Strategia CPU: prima correttezza, poi accelerazione misurata

Moira deve rimanere un riferimento operativo e una base di confronto per lo sviluppo del kaffeine JIT. Il confronto con l’interprete va affiancato a test con risultati attesi indipendenti e, quando necessario, a verifiche sul sistema di riferimento.

Il JIT deve accelerare l’esecuzione senza ridurre silenziosamente la compatibilità. Registri, flag, memoria, callback e risultati osservabili devono rispettare il modello CPU e i contratti dichiarati.

Un eventuale fallback fra JIT e interprete richiede un progetto esplicito per lo stato condiviso e le transizioni. Non presumere che copiare D0-D7, A0-A7, PC e SR sia sufficiente in ogni situazione. Stack separati, stato delle eccezioni, prefetch, registri di controllo ed eventuale FPU devono essere considerati nel perimetro pertinente.

La gestione del codice modificato deve distinguere l’invalidazione delle traduzioni guest dalla pubblicazione delle nuove istruzioni host nella cache della CPU.

Le prestazioni devono essere misurate su programmi reali oltre che su microbenchmark, includendo avvio a freddo, esecuzione a caldo, tempo di compilazione, memoria occupata e costo dei passaggi fra codice guest e librerie native.

L’obiettivo è rendere più veloci le applicazioni mantenendone corretto il comportamento, non massimizzare un benchmark isolato.

## 6. Espansione futura: PPC e altri ambienti Amiga

A lungo termine, kaffeine deve poter ospitare più motori CPU e più ambienti di compatibilità.

Una possibile evoluzione è il supporto al codice PowerPC, partendo da un perimetro circoscritto come WarpOS e valutando separatamente PowerUP. L’eventuale compatibilità con applicazioni MorphOS o AmigaOS 4 deve essere trattata come un ulteriore progetto, con proprie dipendenze, ABI e servizi.

Emulare una CPU non equivale a implementare il sistema operativo che un programma si aspetta. Non dichiarare compatibilità con una famiglia di applicazioni sulla sola base dell’esecuzione delle sue istruzioni.

Le componenti condivisibili, come gestione della memoria, servizi JIT, conversioni e amministrazione delle risorse, devono poter essere riutilizzate. Lo stato CPU e le convenzioni di chiamata specifiche devono invece restare distinti: il PowerPC non va forzato dentro un’interfaccia concepita esclusivamente per i registri 68k.

Il multicore è un’opportunità futura, non una proprietà automatica. L’esecuzione concorrente dei motori richiederà regole precise per memoria condivisa, atomiche, messaggi, callback e durata degli oggetti. L’affinità a un core deve essere una scelta motivata, non un requisito imposto preventivamente.

Questa visione non autorizza ad avviare ora tutti questi filoni. Le attività immediate devono restare concentrate sul perimetro concordato.

## 7. Roadmap orientata a risultati osservabili

Le fasi possono sovrapporsi quando esistono attività indipendenti, ma ogni avanzamento deve avere criteri verificabili.

| Fase | Risultato atteso |
| --- | --- |
| Compatibilità 68k su AROS ARM64 | Applicazioni CLI e grafiche reali completano operazioni utili e rilasciano correttamente le risorse. Personal Paint è il riferimento grafico prioritario. |
| Accelerazione con kaffeine JIT | Gli stessi binari funzionano con il JIT o con una modalità ibrida verificata, con benefici misurati e senza regressioni rispetto alla base interpretata. |
| Consolidamento su hardware reale | Interazione, stabilità, memoria e prestazioni sono verificate sulle macchine supportate, non soltanto in QEMU. |
| Portabilità verso altri host | La base comune viene compilata e collaudata su un secondo host, con RISC-V come candidato, iniziando dall’interprete prima dell’eventuale JIT dedicato. |
| Ulteriori ambienti guest | Un motore PPC e un primo ambiente di compatibilità circoscritto eseguono software reale, riutilizzando l’infrastruttura comune. |


L’ordine delle singole PR deve seguire i blocchi reali osservati nelle applicazioni e i rischi architetturali ancora da verificare, non soltanto il numero di funzioni implementabili.

## 8. Regole di sviluppo e di verifica

Ogni modifica deve dichiarare il contratto che implementa, il difetto che corregge, le prove eseguite e i limiti rimasti. Privilegiare PR circoscritte e regressioni che falliscano sull’implementazione precedente.

Distinguere sempre fra test host con API simulate, esecuzione effettiva del codice JIT, prove dentro AROS in QEMU, prove su hardware reale e confronti con il sistema Amiga di riferimento.

La verifica deve comprendere, dove pertinente, errori e cleanup, identità dei puntatori, sincronizzazione bidirezionale degli oggetti, callback annidate e durata delle risorse. Un programma che apre una finestra non è ancora un’applicazione pienamente funzionante.

Registrare gli esiti negativi attesi senza confonderli con regressioni. Conservare revisione dei sorgenti, hash dei binari, configurazione del sistema e risultati riproducibili. Una CI verde che salta i test AArch64 non certifica l’esecuzione del JIT AArch64.

Il numero di thunk, asserzioni o commit non è una percentuale di compatibilità. La misura principale è ciò che i programmi riescono a fare correttamente.

## 9. Una piattaforma aperta, costruita per durare

Il sistema di compatibilità deve restare open source, documentato e contribuibile. Preservare attribuzioni, provenienza e licenze dei componenti utilizzati. Le applicazioni eseguite mantengono le proprie licenze; il progetto non deve dipendere dalla redistribuzione non autorizzata di software proprietario.

L’ambizione di accogliere tutto l’ecosistema non deve trasformarsi nella promessa che qualsiasi programma funzionerà senza limiti. Per software che richiede hardware specifico o servizi non disponibili, valutare separatamente se servano ulteriori componenti di compatibilità o un ambiente più isolato.

Il fine non è cancellare le differenze fra le piattaforme Amiga, ma impedire che quelle differenze rendano inutilizzabile il loro software nel futuro.

## Principio guida per ogni decisione

Costruire oggi ciò che serve a far funzionare davvero le applicazioni, senza legare la compatibilità di domani a una sola CPU, a un solo motore o a una sola persona.

**Kaffeine: una casa open source per tutte le anime dell’Amiga.**
