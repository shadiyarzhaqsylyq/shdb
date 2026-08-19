package main

import "core:fmt"



Relation :: struct {
	id:          u32,
	name:        string,
	cardinality: f64,
}



main :: proc() {
	// Define base relations with cardinalities
	relations := []Relation{
		{id = 0, name = "R1",     cardinality = 50}, // cardinatliy = number of Tables
		{id = 1, name = "R2",    cardinality = 100},
		{id = 2, name = "R3", cardinality = 30},
		{id = 3, name = "R4",  cardinality = 40},
	}

	
}
