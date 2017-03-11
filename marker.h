//
// Copyright © 2017 by Yuri Victorovich. All rights reserved.
//

uint8_t markerMaxSizeBytes(unsigned numParts, unsigned msgSize);
uint8_t markerPrint(uint64_t id, unsigned partNo, unsigned numParts, unsigned off, unsigned sz, uint8_t *marker);
int markerExists(const uint8_t *message, size_t length);
uint8_t markerParse(const uint8_t *message, size_t length,
                    uint64_t *id,
                    unsigned *partNo, unsigned *numParts, unsigned *off, unsigned *sz);
