
/*!
 * \file low_cache.c
 * 
 * \author Thibaut SORIANO
 * \author Natiora Rapenomanana
 * \contributors Lucas SOUMILLE Pascal TUNG
 */

#include "low_cache.h"
#include "cache.h"
#include "strategy.h"

int cptSynchro;

 //! Retourne le premier bloc libre ou NULL si le cache est plein
struct Cache_Block_Header *Get_Free_Block(struct Cache *pcache)
{
    struct Cache_Block_Header *libre = pcache->pfree;
    // On parcours tous les autres block restante pour trouver un bloc 'free'
    for(int i = 0; i < pcache->nblocks; i++) {
        if((pcache->headers[i].flags & VALID)==0) {
            libre = &pcache->headers[i];
            break;
        }
    }

    libre->flags |= VALID;
    
    return libre;
}

void checkSynchronisation(struct Cache *pcache){
    if(++cptSynchro == NSYNC)
        Cache_Sync(pcache);
}

//! Création du cache.
struct Cache *Cache_Create(const char *fic, unsigned nblocks, unsigned nrecords,
                           size_t recordsz, unsigned nderef){
    //creation de la struct
    struct Cache * cache = malloc(sizeof(struct Cache));
    cache->file = fic;
    cache->fp = fopen(fic, "rb+");
    cache->nblocks = nblocks;
    cache->nderef = nderef;
    cache->blocksz = recordsz * nrecords;
    cache->nrecords = nrecords;
    cache->recordsz = recordsz;
    cache->pstrategy = Strategy_Create(cache);
    //cache->instrument = (struct Cache_Instrument *)malloc(sizeof(struct Cache_Instrument));
    Cache_Get_Instrument(cache);
    struct Cache_Block_Header * headers = malloc(sizeof(struct Cache_Block_Header) * nblocks);
    cache->headers = headers;
    //initialisation des headers
    for(int i = 0 ; i < nblocks ; ++i){
		cache->headers[i].ibcache = i;
		cache->headers[i].flags = 0;
		cache->headers[i].data = malloc(nrecords * recordsz);
    }
    //initialisation du premier block free
    cache->pfree = Get_Free_Block(cache);

    cptSynchro = 0;

    return cache;
}

//! Fermeture (destruction) du cache.
Cache_Error Cache_Close(struct Cache *pcache){
    //synchronise cache / fichier
    if(Cache_Sync(pcache) == CACHE_KO) return CACHE_KO;
    //close le fichier
    if(fclose(pcache->fp) == CACHE_KO) return CACHE_KO;
    //desalloue 
    Strategy_Close(pcache);

    //on free les datas
    for(int i = 0 ; i < pcache->nblocks ; ++i){
    	free(pcache->headers[i].data);
    }

    free(&pcache->instrument);
    free(pcache->headers);
    free(pcache);
    return CACHE_OK;
}

//! Synchronisation du cache.
Cache_Error Cache_Sync(struct Cache *pcache){
	//reinit cpt de synchronisation
	cptSynchro = 0;
    //+1 au nombre de synchronisation
    pcache->instrument.n_syncs++;
    for(int i = 0 ; i < pcache->nblocks ; i++){
        //on regarde si il a été modifié
        if((pcache->headers[i].flags & MODIF) > 0){
            if(fseek(pcache->fp, pcache->headers[i].ibfile * pcache->blocksz, SEEK_SET) != 0) return CACHE_KO;
            if(fputs(pcache->headers[i].data, pcache->fp) == EOF) return CACHE_KO;
            //on remet le bit a modification a 0
            pcache->headers[i].flags &= ~MODIF;
        } 
    }
    return CACHE_OK;
}


//! Invalidation du cache.
Cache_Error Cache_Invalidate(struct Cache *pcache){
    for(int i = 0 ; i < pcache->nblocks ; i++)
        pcache->headers[i].flags &= ~VALID;

    pcache->pfree = Get_Free_Block(pcache);

    Strategy_Invalidate(pcache);
    return CACHE_OK;
}

struct Cache_Block_Header * getBlockByIbfile(struct Cache *pcache, int irfile){
    int ibSearch = irfile / pcache->nrecords; // Indice du bloc contenant l'enregistrement
    for(int i = 0 ; i < pcache->nblocks ; ++i){
    	if(pcache->headers[i].ibfile == ibSearch)
    		return &pcache->headers[i]; // Bloc ayant possiblement l'enregistrement
    }
    return NULL;
}

//! Lecture  (à travers le cache).
Cache_Error Cache_Read(struct Cache *pcache, int irfile, void *precord){
    struct Cache_Block_Header * header = getBlockByIbfile(pcache, irfile);
    //si le block n'est pas dans le cache
    if(header == NULL || (header->flags &= VALID) == 0){
        header = Strategy_Replace_Block(pcache);
        header->ibfile = irfile / pcache->nrecords;
        if(fseek(pcache->fp, DADDR(pcache, header->ibfile), SEEK_SET) != 0) return CACHE_KO;
        if(fgets(header->data, pcache->blocksz, pcache->fp) == EOF) return CACHE_KO; 
        //MAJ des flags
        header->flags |= VALID; //n'est plus free
		header->flags &= ~MODIF; //mets le flag de modification
    } else {
    	//l'élément est dans le cache
    	pcache->instrument.n_hits++;
    }
    //on copie dans le buffer
    if(fgets((char *)precord, pcache->recordsz, ADDR(pcache, irfile, header))== EOF) return CACHE_KO;
    //+1 au nombre de lecture
    pcache->instrument.n_reads++;
    checkSynchronisation(pcache);
    Strategy_Read(pcache, header);
    return CACHE_OK;
}

//! Écriture (à travers le cache).
Cache_Error Cache_Write(struct Cache *pcache, int irfile, const void *precord){
	struct Cache_Block_Header * header = getBlockByIbfile(pcache, irfile);
	//si le block n'est pas dans le cache
	if(header == NULL || (header->flags &= VALID) == 0){
		header = Strategy_Replace_Block(pcache);
        header->ibfile = irfile / pcache->nrecords;
        if(fseek(pcache->fp, DADDR(pcache, header->ibfile), SEEK_SET) != 0) return CACHE_KO;
        if(fgets(header->data, pcache->blocksz, pcache->fp) == EOF) return CACHE_KO; 
        //MAJ des flags
        header->flags |= VALID; //n'est plus free
		header->flags &= ~MODIF; //mets le flag de modification
	} else {
		//l'élement est dans le cache
		pcache->instrument.n_hits++;
	}
	//on copie dans le bloc
    if(fgets(ADDR(pcache, irfile, header), pcache->recordsz, precord)== EOF) return CACHE_KO;
    
	header->flags &= ~MODIF;
	//+1 au nombre d'écriture
	pcache->instrument.n_reads++;
	Cache_Sync(pcache);
	checkSynchronisation(pcache);
	Strategy_Write(pcache, header);
	return CACHE_OK;
}

struct Cache_Instrument *Cache_Get_Instrument(struct Cache *pcache)
{
    struct Cache_Instrument stat = pcache->instrument;
    pcache->instrument.n_reads   = 0;
    pcache->instrument.n_writes  = 0;
    pcache->instrument.n_hits    = 0;
    pcache->instrument.n_syncs   = 0;
    pcache->instrument.n_deref   = 0;
    return &stat;
}
