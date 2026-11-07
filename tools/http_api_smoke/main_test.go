package main

import (
	"reflect"
	"testing"
)

func TestArgumentsAndProfiles(t *testing.T) {
	for _, test := range []struct {
		name      string
		arguments []string
		maximum   int
		profiles  []int
		label     string
	}{
		{"legacy", []string{"http://device/"}, 4, []int{2, 4}, "2/4"},
		{"explicit-four", []string{"--max-concurrent-clients=4", "http://device/"}, 4, []int{2, 4}, "2/4"},
		{"budget-two", []string{"--max-concurrent-clients=2", "http://device/"}, 2, []int{2}, "2"},
	} {
		t.Run(test.name, func(t *testing.T) {
			base, maximum, err := parseArguments(test.arguments)
			if err != nil || base != "http://device" || maximum != test.maximum {
				t.Fatalf("parse = %q, %d, %v", base, maximum, err)
			}
			if profiles := concurrentProfiles(maximum); !reflect.DeepEqual(profiles, test.profiles) {
				t.Fatalf("profiles = %v, want %v", profiles, test.profiles)
			}
			if label := profileLabel(maximum); label != test.label {
				t.Fatalf("label = %q, want %q", label, test.label)
			}
		})
	}
}

func TestInvalidArguments(t *testing.T) {
	for _, arguments := range [][]string{
		nil, {"http://one", "http://two"},
		{"--max-concurrent-clients=1", "http://device"},
		{"--max-concurrent-clients=3", "http://device"},
		{"--max-concurrent-clients=8", "http://device"},
		{"--max-concurrent-clients=bad", "http://device"},
		{"--unknown", "http://device"},
		{"http://device", "--max-concurrent-clients=2"},
	} {
		if _, _, err := parseArguments(arguments); err == nil {
			t.Fatalf("accepted invalid arguments %q", arguments)
		}
	}
}
