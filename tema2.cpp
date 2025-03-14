#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <map>

using namespace std;

#define TRACKER_RANK 0
#define MAX_FILES 10
#define MAX_FILENAME 15
#define HASH_SIZE 32
#define MAX_CHUNKS 100

struct ClientFile{
    char name[MAX_FILENAME];
    int nr_hashes;
    char hashes[MAX_CHUNKS][HASH_SIZE + 1];
};

struct Download_Info{
    int rank;
    vector<string> wanted;
    vector<ClientFile>* owned;
};

struct Upload_Info{
    int rank;
    vector<ClientFile> owned;
};

// functie luata si modificata din laborator
void create_customType(MPI_Datatype &customtype){
    // folosim un tip MPI personalizat pentru a defini structura ClientFile
    MPI_Datatype oldtypes[3];
    int blockcounts[3];
    MPI_Aint offsets[3];

    // campul name
    offsets[0] = offsetof(ClientFile, name);
    oldtypes[0] = MPI_CHAR;
    blockcounts[0] = MAX_FILENAME;
 
    // campul nr_hashes
    offsets[1] = offsetof(ClientFile, nr_hashes);
    oldtypes[1] = MPI_INT;
    blockcounts[1] = 1;
 
    // campul hashes
    offsets[2] = offsetof(ClientFile, hashes);
    oldtypes[2] = MPI_CHAR;
    blockcounts[2] = MAX_CHUNKS * (HASH_SIZE + 1);

    // cream structura personalizata folosindu ne de structura
    MPI_Type_create_struct(3, blockcounts, offsets, oldtypes, &customtype);
    MPI_Type_commit(&customtype);
}

// functie folosita la citirea din fisier a clientilor
void read_file(int rank, vector<ClientFile>&owned, vector<string>&wanted) {
    string filename = "in"+to_string(rank)+".txt";
    ifstream f(filename);
    if (!f.is_open()) {
        cerr << "Error opening file " << filename << "!\n";
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    // citim numarul  de fisiere detinute de client
    int nrOwned;
    f >> nrOwned;
    // pentru fiecare fisier detinut de client
    for(int i = 0; i < nrOwned; i++){
        ClientFile file;
        // citim numele fisierului detinut
        f >> file.name;
        // citim numarul se segmente ale fisierului
        f >> file.nr_hashes;
        // citim segmentele
        for(int j = 0; j < file.nr_hashes; j++){
            char hash[32];
            f >> hash;
            strncpy(file.hashes[j], hash, HASH_SIZE + 1);
            file.hashes[j][HASH_SIZE + 1] = '\0';
        }
        owned.push_back(file);
    }
    // citim numarul de fisiere dorite de client
    int nrWanted;
    f >> nrWanted;
    // citim numele fisierelor dorite
    for (int i = 0; i < nrWanted; i++){
        string wanted_filename;
        f >> wanted_filename;
        wanted.push_back(wanted_filename);
    }
    f.close();
}

// functie folosita in tracker
// verifica daca un hash a fost adaugat deja in lista de hash uri
bool custom_find(const vector<string>& v, const char* str) {
    for (const auto& s : v) {
        if (s == str) {
            return true;
        }
    }
    return false;
}

// functie folosita in download
// actualizeaza fisierele detinute de client
void update_owned(vector<ClientFile>& owned,const string& filename, const string& hash){
    bool file_exists = false;
    // parcurge fisierele detinute pentru a gasi un fisier specific
    for(auto& file : owned){
        if (strcmp(file.name, filename.c_str())==0){
            // daca l a gasit, adauga un nou hash
            strcpy(file.hashes[file.nr_hashes], hash.c_str());
            file.nr_hashes++;
            file_exists = true;
            break;
        }
    }
    // daca nu l a gasit, creeaza un nou fisier
    if (file_exists == false){
        ClientFile file;
        strcpy(file.name, filename.c_str());
        // adauga hash ul pe prima pozitie
        file.nr_hashes = 1;
        strcpy(file.hashes[0], hash.c_str());
    }
}

// functie folosita in download
// creeaza un fisier cu hash urile obtinute conform cerintei
void create_file(map<int, string> obtained_hashes, int rank, string filename){
    // creeaza numele fisierului conform cerintei
    string output_name = "client" + to_string(rank) + "_" + filename;
    ofstream output(output_name);
    if (!output.is_open()) {
        cerr << "Error opening file " << output_name << "!\n";
        return;
    }
    // scrie hash urile obtinute in fisier
    for(const auto& [index, hash] : obtained_hashes){
        output << hash << "\n";
    }
    cout<<"Fisierul " << output_name<<" a fost creat cu succes!"<<endl;
    output.close();
}

// functie folosita in download
// verifica daca un hash a fost deja descarcat
bool has_hash(const map<int, string>& obtained_hashes, const string& hash) {
    // merge prin toate hash urile detinute
    for (const auto& [index, h] : obtained_hashes) {
        // verifica hash ul
        if (h == hash) {
            return true;
        }
    }
    return false;
}

// functie folosita in upload
// verifica daca un peer detine un anumit hash
bool peer_has_hash(const vector<ClientFile>& owned, const char* filename, const char* hash){
    // verificam toate fisierele detinute
    for(const auto& file : owned){
        if (strcmp(file.name, filename)==0){
            // verificam fiecare hash din fisier
            for(int i = 0; i < file.nr_hashes; i++){
                if (strcmp(file.hashes[i], hash) == 0){
                        return true;
                }
            }
        }
    }
    return false;
}

void *download_thread_func(void *arg)
{
    // extragem datele necesare din structura
    Download_Info *data = (Download_Info*)arg;
    int rank = data->rank;
    vector<string> wanted = data->wanted; // fisierele dorite
    vector<ClientFile> owned = *(data->owned); // fisierele detinute
    // informatii despre seeds/peers
    int swarm_size_peers;
    int swarm_size_seeds;
    set<int> peers;
    set<int> seeds;
    vector<int> combined_sources; // stocam toti seed si peer
    map<int, string> obtained_hashes; // stocam hash urile obtinute
    // contoare pentru algoritmul round-robin
    int round_robin_index; // index pentru alegerea seed/peer
    int received_hashes; // contor pentru hash urile primite

    // parcurgem toate fisierele dorite de client
    for(size_t i = 0; i < wanted.size(); i++){
        // in cazul in care clientul nu doreste niciun fisier
        // trimite un mesaj catre tracker in locul unui nume de fisier
        if (i == 0 && i == wanted.size()){
            MPI_Send("NO_REQUEST", MAX_FILENAME, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
            break;
        }
        // in cazul in care clientul doreste un fisier
        // trimitem numele fisierului dorit catre tracker
        MPI_Send(wanted[i].c_str(), MAX_FILENAME, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
        // clientul primeste numarul de seeds
        MPI_Recv(&swarm_size_seeds, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        // clientul primeste fiecare seed, care vor fi stocati in setul seeds
        for(int j =0; j< swarm_size_seeds; j++){
            int seed;
            MPI_Recv(&seed, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // nu stocam clientul curent in caz ca exista ca seed pentru fisierul dorit
            if (seed != rank){
                seeds.insert(seed);
            }
        }
        // analog peers
        MPI_Recv(&swarm_size_peers, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        for(int j =0; j< swarm_size_peers; j++){
            int peer;
            MPI_Recv(&peer, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (peer != rank){
                peers.insert(peer);
            }
        }
        // clientul primeste de la tracker numarul de hash uri si hash urile fisierului dorit
        int num_hashes;
        vector<string> file_hashes;
        MPI_Recv(&num_hashes, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        for(int j = 0; j < num_hashes; j++){
            char hash[HASH_SIZE + 1];
            MPI_Recv(&hash, HASH_SIZE + 1, MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            file_hashes.emplace_back(hash);
        }
        // initializam variabilele pentru algoritmul round-robin
        round_robin_index = 0;
        received_hashes = 0;
        // combinam seeds si peers intr-un vector
        combined_sources.insert(combined_sources.end(), peers.begin(), peers.end());
        combined_sources.insert(combined_sources.end(), seeds.begin(), seeds.end());
        // continuam sa cerem hash uri pana cand le primim pe toate
        while(received_hashes < num_hashes){
            // parcurgem toate hash urile fisierului
            for(int wanted_hash = 0; wanted_hash < num_hashes; wanted_hash++){
                int ack = 0;
                int chosen_source;
                // daca am downloadat 10 segmente, atunci actualizam swarm urile
                if (received_hashes % 10 == 0){
                    // cerem acces la tracker
                    int request_Tracker = 1;
                    MPI_Send(&request_Tracker, 1, MPI_INT, 0, 10, MPI_COMM_WORLD);
                    // asteptam sa primim acces de la tracker
                    int grant_from_tracker = 0;
                    MPI_Recv(&grant_from_tracker, 1, MPI_INT, 0, 11, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    // cerem acces la tracker pana primim
                    if (grant_from_tracker != 1){
                        wanted_hash--;
                        continue;
                    }
                    // trimitem numele fisierului dorit catre tracker
                    MPI_Send(wanted[i].c_str(), MAX_FILENAME, MPI_CHAR, 0, 0, MPI_COMM_WORLD);
                    // asteptam sa primim nuamrul de seeds
                    MPI_Recv(&swarm_size_seeds, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    for(int j =0; j< swarm_size_seeds; j++){
                        int seed;
                        // asteptam sa primim fiecare seed
                        MPI_Recv(&seed, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                        // daca seed ul se afla deja in vectorul cu totii seed/peer, nu il mai adaugam
                        auto element = find(combined_sources.begin(), combined_sources.end(), seed);
                        if (element == combined_sources.end() && seed != rank) {
                            combined_sources.push_back(seed);
                        }
                    }
                    // analog peers
                    MPI_Recv(&swarm_size_peers, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                    for(int j =0; j< swarm_size_peers; j++){
                        int peer;
                        MPI_Recv(&peer, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                        auto element = find(combined_sources.begin(), combined_sources.end(), peer);
                        if (element == combined_sources.end() && peer != rank) {
                            combined_sources.push_back(peer);
                        }
                    }
                    // eliberam accesul la tracker
                    int release_tracker = 1;
                    MPI_Send(&release_tracker, 1, MPI_INT, 0, 12, MPI_COMM_WORLD); 
                }
                // verificam daca am obtinut deja hash ul curent
                if (has_hash(obtained_hashes, file_hashes[wanted_hash]) == true){
                    continue;
                }
                // daca am parcurs toti seed/peer, incepem de la capat
                if (round_robin_index >= (int)combined_sources.size()){
                    round_robin_index = 0;
                }
                // alegem seed/peer
                chosen_source = combined_sources[round_robin_index];
                // cerem acces la seed/peer ul dorit
                int request_Peer = 1;
                MPI_Send(&request_Peer, 1, MPI_INT, chosen_source, 20, MPI_COMM_WORLD); 
                // asteptam sa primim acces
                int grant_from_peer = 0;
                MPI_Recv(&grant_from_peer, 1, MPI_INT, chosen_source, 21, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                // cerem acces la acest seed/peer pana primim
                if (grant_from_peer != 1){
                    wanted_hash--;
                    continue;
                }
                // trimitem numele fisierului dorit catre seed/peer
                MPI_Send(wanted[i].c_str(), MAX_FILENAME, MPI_CHAR, chosen_source, 1, MPI_COMM_WORLD);
                // trimitem hash ul dorit catre seed/peer
                MPI_Send(file_hashes[wanted_hash].c_str(), HASH_SIZE + 1, MPI_CHAR, chosen_source, 1, MPI_COMM_WORLD);
                // asteptam sa vedem daca seed/peer ul are hash ul dorit de client
                MPI_Recv(&ack, 1, MPI_INT, chosen_source, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
                // daca seed/peer are hash ul dorit
                if (ack == 1){
                    // crestem numarul de hash uri obtinute
                    received_hashes++;
                    // stocam hash ul
                    obtained_hashes[wanted_hash] = file_hashes[wanted_hash];
                    // updatam hahs urile detinute de client
                    update_owned(owned, wanted[i], file_hashes[wanted_hash]);
                }
                // eliberam accesul la seed/peer ales
                int release_peer = 1;
                MPI_Send(&release_peer, 1, MPI_INT, chosen_source, 22, MPI_COMM_WORLD);
                // incrementam indexul pentru alegerea urmatorului seed/peer
                round_robin_index++;
            }
        }
        // clientul trimite mesaj tracker ului ca a terminat de downloadat fisierul dorit
        int file_done = 1;
        MPI_Send(&file_done,1,MPI_INT,0,2,MPI_COMM_WORLD);
        // cream file ul cerut cu numele clientului, numele fisierului si hash urile obtinute
        create_file(obtained_hashes, rank, wanted[i]);
        // golim variabilele pentru urmatorul fisier dorit
        obtained_hashes.clear();
        seeds.clear();
        peers.clear();
        combined_sources.clear();
    }
    // clientul trimite mesaj tracker ului ca a terminat de downloadat toate fisierele dorite
    int done = 1;
    MPI_Send(&done, 1, MPI_INT, 0, 3, MPI_COMM_WORLD);
    return NULL;
}

void *upload_thread_func(void *arg)
{
    // extragem datele necesare din structura
    Upload_Info *data = (Upload_Info*)arg;
    vector<ClientFile> owned = data->owned; // fisierele detinute
    bool locked_peer = false; // variabila care defineste accesul la peer
    bool stop = false; // variabila primita de la tracker care indica daca clientul a terminat cu totul
    int source; // clientul care trimite mesaj seed/peer ului
    int tag; // tag ul mesajului
    int grant_access;
    char filename[MAX_FILENAME];
    int ack;
    // continua pana cand tracker ul ii spune sa se opreasca
    while(!stop){
        char hash[32];
        MPI_Status status;
        // see/peer asteapta un mesaj de la oricine (tracker sau client), tag ul nu conteaza
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        source = status.MPI_SOURCE;
        tag = status.MPI_TAG;
        // cazul in care seed/peer ului i se cere accesul la acesta de la un client
        if (tag == 20){
            // primeste cererea de acces
            int access_request;
            MPI_Recv(&access_request, 1, MPI_INT, source, 20, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // daca nu este deja blocat, atunci ii da acces clientului
            if(locked_peer == false){
                grant_access = 1;
                locked_peer = true;
            } else {
                grant_access = 0;
            }
            MPI_Send(&grant_access, 1, MPI_INT, source, 21, MPI_COMM_WORLD);
        }
        // cazul in care clientul elibereaza accesul la seed/peer
        else if (tag == 22){
            // seed/peer ul primeste cererea de eliberare
            int release_request;
            MPI_Recv(&release_request, 1, MPI_INT, source, 22, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // elibereaza accesul
            locked_peer = false;
        }
        // cazul in care seed/peer ului i se cere un segment
        else if (tag == 1){
            // seed/peer ul primeste numele fisierului
            MPI_Recv(&filename, MAX_FILENAME, MPI_CHAR, source, 1, MPI_COMM_WORLD, &status);
            // primeste hash ul cerut
            MPI_Recv(&hash, HASH_SIZE + 1, MPI_CHAR, source, 1, MPI_COMM_WORLD, &status);
            // verifica sa vada daca il are
            bool found = peer_has_hash(owned, filename, hash);
            // trimite raspunsul clientului care i a cerut hash ul
            if (found == true){
                ack = 1;
            } else {
                ack = 0;
            }
            MPI_Send(&ack, 1, MPI_INT, source, 2, MPI_COMM_WORLD);
        }
        // cazul in care tracker ul ii spune clientului sa se opreasca cu totul
        else if (tag == 4){
            MPI_Recv(&stop, 1, MPI_INT, source, 4, MPI_COMM_WORLD, &status);
        }
    }
    return NULL;
}

void tracker(int numtasks, int rank) {
    unordered_map<string, vector<string>> hashes; // hash urile pentru fiecare fisier
    unordered_map<string,set<int>> track_peers; // peers pentru fiecare fisier
    unordered_map<string,set<int>> track_seeds; // seeds pentru fiecare fisier
    bool locked_tracker = false; // variabila care defineste accesul la tracker
    int all_done = 0; // variabila care indica daca toti clientii au terminat
    MPI_Status status;
    // tracker ul primeste informatiile de la fiecare client
    for (int client_rank = 1; client_rank < numtasks; client_rank++){
        MPI_Datatype customtype;
        create_customType(customtype); // tip MPI personalizat pentru fiecare client
        // tracker ul primeste numarul de fisiere detinute de client
        int nr_owned;
        MPI_Recv(&nr_owned, 1, MPI_INT, client_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        // tracker ul primeste fiecare fisier detinut de client
        for(int i = 0; i < nr_owned; i++){
            ClientFile file_data;
            MPI_Recv(&file_data, 1, customtype, client_rank, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // retinem seeds pentru fiecare fisier
            track_seeds[file_data.name].insert(client_rank);
            // retinem hash urile pentru fiecare fisier
            // deoarece clientii trebuie sa stie ce hash uri sa ceara
            // clientii NU vor descarca hash uri de la tracker
            for(int j = 0; j < file_data.nr_hashes; j++){
                if (!custom_find(hashes[file_data.name], file_data.hashes[j])) {
                    hashes[file_data.name].push_back(file_data.hashes[j]);
                }
            }
        }
        MPI_Type_free(&customtype);
    }
    // dupa ce a primit informatiile de la toti clientii, tracker ul trimite un ack
    // adica poate incepe comunicarea intre clienti
    for(int client_rank = 1; client_rank < numtasks; client_rank++){
        int ack = 1;
        MPI_Send(&ack, 1, MPI_INT, client_rank, 0, MPI_COMM_WORLD);
    }
    char filename[MAX_FILENAME];
    int source = status.MPI_SOURCE;
    int tag = status.MPI_TAG;
    int swarm_size_seeds;
    int swarm_size_peers;
    // continuam pana cand toti clientii termina de descarcat
    while(all_done < numtasks - 1){
        // tracker ul asteapta un mesaj de la orice client, tag ul nu conteaza
        MPI_Probe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);
        source = status.MPI_SOURCE;
        tag = status.MPI_TAG;
        // cazul in care un client cere informatii de la tarcker
        if (tag == 0){
            // tracker ul primeste numele fisierului
            MPI_Recv(&filename, MAX_FILENAME, MPI_CHAR, source, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            if (string(filename) == "NO_REQUEST"){
                continue;
            }
            // timite numarul de seeds
            swarm_size_seeds = (int)track_seeds[filename].size();
            MPI_Send(&swarm_size_seeds, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
            // trimite fiecare seed
            for(int seed : track_seeds[filename]){
                MPI_Send(&seed, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
            }
            // analog seeds
            swarm_size_peers = (int)track_peers[filename].size();
            MPI_Send(&swarm_size_peers, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
            if(track_peers[filename].size() > 0){
                for(int peer : track_peers[filename]){
                    MPI_Send(&peer, 1, MPI_INT, source, 0, MPI_COMM_WORLD);                    
                }
            }
            // daca clientul care a cerut datele nu se afla in peers
            // atunci se adauga si i se trimit hash urile
            // pentru a sti ce sa ceara mai departe in download
            // clientu NU va descarca segmente de la tracker
            if (track_peers[filename].count(source) <= 0){
                track_peers[filename].insert(source);
                vector<string> file_hashes = hashes[filename];
                // numarul de hash uri
                int num_hashes = (int)file_hashes.size();
                MPI_Send(&num_hashes, 1, MPI_INT, source, 0, MPI_COMM_WORLD);
                // hahs urile in sine
                for (const string& hash : file_hashes) {
                   MPI_Send(hash.c_str(), HASH_SIZE + 1, MPI_CHAR, source, 0, MPI_COMM_WORLD);
                }
            }
        }
        // cazul in care un client a terminat un fisier
        else if (tag == 2) {
            // primeste semnalul de la client ca a terminat fisierul
            int file_done=0;
            MPI_Recv(&file_done, 1, MPI_INT, source, 2, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // scoate clientul din swarm ul de peers pentru fisierul respectiv
            // si il adauga in swarm ul de seeds
            if (file_done == 1){
                track_seeds[filename].insert(source);
                track_peers[filename].erase(source);
            }
        }
        // cazul in care un client a terminat de descarcat toate fisierele
        else if (tag == 3) {
            // primeste semnalul de la client
            int done=0;
            MPI_Recv(&done, 1, MPI_INT, source, 3, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // incrementeaza contorul pentru clientii care au terminat
            if (done == 1){
                ++all_done;
            }
        }
        // cazul in care un client cere acces la tracker
        else if (tag == 10) {
            // primeste cererea de acces
            int access_request;
            MPI_Recv(&access_request, 1, MPI_INT, source, 10, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // daca nu este deja blocat, atunci ii da acces clientului si se blocheaza
            int grant_access;
            if (locked_tracker == false){
                grant_access = 1;
                locked_tracker = true;
            } else {
                grant_access = 0;
            }
            MPI_Send(&grant_access, 1, MPI_INT, source, 11, MPI_COMM_WORLD);
        }
        // cazul in care un client elibereaza accesul la tracker
        else if (tag == 12) {
            int release_request;
            MPI_Recv(&release_request, 1, MPI_INT, source, 12, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            // deblocheaza tracker ul
            locked_tracker = false;
        }
    }
    // daca toti clientii au terminat
    if (all_done >= numtasks-1) {
        bool stop = true;
        // acesta trimite tuturor clientilor un mesaj de oprire
        for(int client_rank = 1; client_rank < numtasks; client_rank++){
            MPI_Send(&stop, 1, MPI_INT, client_rank, 4, MPI_COMM_WORLD);
        }
    }
}

void peer(int numtasks, int rank) {
    pthread_t download_thread;
    pthread_t upload_thread;
    void *status;
    int r;
    vector<ClientFile> owned; // fisiere detinute de client
    vector<string> wanted; // fisiere vrute de client
    // se creaza un tip MPI personalizat
    MPI_Datatype customtype;
    create_customType(customtype);
    // citim datele clientului din fisier
    read_file(rank, owned, wanted);
    // trimitem datele tracker ului
    // numarul de fisiere detinute
    int nr_owned = owned.size();
    MPI_Send(&nr_owned, 1, MPI_INT, 0, 0, MPI_COMM_WORLD);
    // datele din structura
    for(const auto& file_data : owned){
        MPI_Send(&file_data, 1, customtype, 0, 0, MPI_COMM_WORLD);
    }
    // clientul asteapta ca si ceilalti clienti sa trimita datele
    int ack;
    MPI_Recv(&ack, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

    Download_Info download_data;
    download_data.rank = rank;
    download_data.wanted = wanted;
    download_data.owned = &owned;

    Upload_Info upload_data;
    upload_data.rank = rank;
    upload_data.owned = owned;

    r = pthread_create(&download_thread, NULL, download_thread_func, (void *) &download_data);
    if (r) {
        printf("Eroare la crearea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_create(&upload_thread, NULL, upload_thread_func, (void *) &upload_data);
    if (r) {
        printf("Eroare la crearea thread-ului de upload\n");
        exit(-1);
    }

    r = pthread_join(download_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de download\n");
        exit(-1);
    }

    r = pthread_join(upload_thread, &status);
    if (r) {
        printf("Eroare la asteptarea thread-ului de upload\n");
        exit(-1);
    }
    MPI_Type_free(&customtype);
}
 
int main (int argc, char *argv[]) {
    int numtasks, rank;
 
    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);
    if (provided < MPI_THREAD_MULTIPLE) {
        fprintf(stderr, "MPI nu are suport pentru multi-threading\n");
        exit(-1);
    }
    MPI_Comm_size(MPI_COMM_WORLD, &numtasks);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == TRACKER_RANK) {
        tracker(numtasks, rank);
    } else {
        peer(numtasks, rank);
    }

    MPI_Finalize();
}
