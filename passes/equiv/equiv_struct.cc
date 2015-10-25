/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2012  Clifford Wolf <clifford@clifford.at>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/yosys.h"
#include "kernel/sigtools.h"

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct EquivStructWorker
{
	Module *module;
	SigMap sigmap;
	SigMap equiv_bits;
	bool mode_fwd;
	bool mode_icells;
	int merge_count;

	struct merge_key_t
	{
		IdString type;
		vector<pair<IdString, Const>> parameters;
		vector<pair<IdString, int>> port_sizes;
		vector<tuple<IdString, int, SigBit>> connections;

		bool operator==(const merge_key_t &other) const {
			return type == other.type && connections == other.connections &&
					parameters == other.parameters && port_sizes == other.port_sizes;
		}

		unsigned int hash() const {
			unsigned int h = mkhash_init;
			h = mkhash(h, mkhash(type));
			h = mkhash(h, mkhash(parameters));
			h = mkhash(h, mkhash(connections));
			return h;
		}
	};

	dict<merge_key_t, pool<IdString>> merge_cache;
	pool<merge_key_t> fwd_merge_cache, bwd_merge_cache;

	void merge_cell_pair(Cell *cell_a, Cell *cell_b)
	{
		SigMap merged_map;
		merge_count++;

		SigSpec inputs_a, inputs_b;
		vector<string> input_names;

		for (auto &port_a : cell_a->connections())
		{
			SigSpec bits_a = sigmap(port_a.second);
			SigSpec bits_b = sigmap(cell_b->getPort(port_a.first));

			log_assert(GetSize(bits_a) == GetSize(bits_b));

			if (!cell_a->output(port_a.first))
				for (int i = 0; i < GetSize(bits_a); i++)
					if (bits_a[i] != bits_b[i]) {
						inputs_a.append(bits_a[i]);
						inputs_b.append(bits_b[i]);
						input_names.push_back(GetSize(bits_a) == 1 ? port_a.first.str() :
								stringf("%s[%d]", log_id(port_a.first), i));
					}
		}

		for (int i = 0; i < GetSize(inputs_a); i++) {
			SigBit bit_a = inputs_a[i], bit_b = inputs_b[i];
			SigBit bit_y = module->addWire(NEW_ID);
			log("      New $equiv for input %s: A: %s, B: %s, Y: %s\n",
					input_names[i].c_str(), log_signal(bit_a), log_signal(bit_b), log_signal(bit_y));
			module->addEquiv(NEW_ID, bit_a, bit_b, bit_y);
			merged_map.add(bit_a, bit_y);
			merged_map.add(bit_b, bit_y);
		}

		std::vector<IdString> outport_names, inport_names;

		for (auto &port_a : cell_a->connections())
			if (cell_a->output(port_a.first))
				outport_names.push_back(port_a.first);
			else
				inport_names.push_back(port_a.first);

		for (auto &pn : inport_names)
			cell_a->setPort(pn, merged_map(sigmap(cell_a->getPort(pn))));

		for (auto &pn : outport_names) {
			SigSpec sig_a = cell_a->getPort(pn);
			SigSpec sig_b = cell_b->getPort(pn);
			module->connect(sig_b, sig_a);
		}

		auto merged_attr = cell_b->get_strpool_attribute("\\equiv_merged");
		merged_attr.insert(log_id(cell_b));
		cell_a->add_strpool_attribute("\\equiv_merged", merged_attr);
		module->remove(cell_b);
	}

	EquivStructWorker(Module *module, bool mode_fwd, bool mode_icells) :
			module(module), sigmap(module), equiv_bits(module),
			mode_fwd(mode_fwd), mode_icells(mode_icells), merge_count(0)
	{
		log("  Starting new iteration.\n");

		pool<SigBit> equiv_inputs;
		pool<IdString> cells;

		for (auto cell : module->selected_cells())
			if (cell->type == "$equiv") {
				SigBit sig_a = sigmap(cell->getPort("\\A").as_bit());
				SigBit sig_b = sigmap(cell->getPort("\\B").as_bit());
				equiv_bits.add(sig_b, sig_a);
				equiv_inputs.insert(sig_a);
				equiv_inputs.insert(sig_b);
				cells.insert(cell->name);
			} else {
				if (mode_icells || module->design->module(cell->type))
					cells.insert(cell->name);
			}

		for (auto cell : module->selected_cells())
			if (cell->type == "$equiv") {
				SigBit sig_a = sigmap(cell->getPort("\\A").as_bit());
				SigBit sig_b = sigmap(cell->getPort("\\B").as_bit());
				SigBit sig_y = sigmap(cell->getPort("\\Y").as_bit());
				if (sig_a == sig_b && equiv_inputs.count(sig_y)) {
					log("    Purging redundant $equiv cell %s.\n", log_id(cell));
					module->remove(cell);
					merge_count++;
				}
			}

		if (merge_count > 0)
			return;

		for (auto cell_name : cells)
		{
			merge_key_t key;
			vector<tuple<IdString, int, SigBit>> fwd_connections;

			Cell *cell = module->cell(cell_name);
			key.type = cell->type;

			for (auto &it : cell->parameters)
				key.parameters.push_back(it);
			std::sort(key.parameters.begin(), key.parameters.end());

			for (auto &it : cell->connections())
				key.port_sizes.push_back(make_pair(it.first, GetSize(it.second)));
			std::sort(key.port_sizes.begin(), key.port_sizes.end());

			for (auto &conn : cell->connections())
			{
				SigSpec sig = equiv_bits(conn.second);

				if (cell->input(conn.first))
					for (int i = 0; i < GetSize(sig); i++)
						fwd_connections.push_back(make_tuple(conn.first, i, sig[i]));

				if (cell->output(conn.first))
					for (int i = 0; i < GetSize(sig); i++) {
						key.connections.clear();
						key.connections.push_back(make_tuple(conn.first, i, sig[i]));

						if (merge_cache.count(key))
							bwd_merge_cache.insert(key);
						merge_cache[key].insert(cell_name);
					}
			}

			std::sort(fwd_connections.begin(), fwd_connections.end());
			key.connections.swap(fwd_connections);

			if (merge_cache.count(key))
				fwd_merge_cache.insert(key);
			merge_cache[key].insert(cell_name);
		}

		for (int phase = 0; phase < 2; phase++)
		{
			auto &queue = phase ? bwd_merge_cache : fwd_merge_cache;

			for (auto &key : queue)
			{
				Cell *gold_cell = nullptr;
				pool<Cell*> cells;

				for (auto cell_name : merge_cache[key]) {
					Cell *c = module->cell(cell_name);
					if (c != nullptr) {
						string n = cell_name.str();
						if (gold_cell == nullptr || (GetSize(n) > 5 && n.substr(GetSize(n)-5) == "_gold"))
							gold_cell = c;
						cells.insert(c);
					}
				}

				if (GetSize(cells) < 2)
					continue;

				for (auto gate_cell : cells)
					if (gate_cell != gold_cell) {
						log("    %s merging cells %s and %s.\n", phase ? "Bwd" : "Fwd", log_id(gold_cell),  log_id(gate_cell));
						merge_cell_pair(gold_cell, gate_cell);
					}
			}

			if (merge_count > 0)
				return;
		}

		log("    Nothing to merge.\n");
	}
};

struct EquivStructPass : public Pass {
	EquivStructPass() : Pass("equiv_struct", "structural equivalence checking") { }
	virtual void help()
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    equiv_struct [options] [selection]\n");
		log("\n");
		log("This command adds additional $equiv cells based on the assumption that the\n");
		log("gold and gate circuit are structurally equivalent. Note that this can introduce\n");
		log("bad $equiv cells in cases where the netlists are not structurally equivalent,\n");
		log("for example when analyzing circuits with cells with commutative inputs. This\n");
		log("command will also de-duplicate gates.\n");
		log("\n");
		log("    -fwd\n");
		log("        by default this command performans forward sweeps until nothing can\n");
		log("        be merged by forwards sweeps, the backward sweeps until forward\n");
		log("        sweeps are effective again. with this option set only forward sweeps\n");
		log("        are performed.\n");
		log("\n");
		log("    -icells\n");
		log("        by default, the internal RTL and gate cell types are ignored. add\n");
		log("        this option to also process those cell types with this command.\n");
		log("\n");
	}
	virtual void execute(std::vector<std::string> args, Design *design)
	{
		bool mode_icells = false;
		bool mode_fwd = false;

		log_header("Executing EQUIV_STRUCT pass.\n");

		size_t argidx;
		for (argidx = 1; argidx < args.size(); argidx++) {
			if (args[argidx] == "-fwd") {
				mode_fwd = true;
				continue;
			}
			if (args[argidx] == "-icells") {
				mode_icells = true;
				continue;
			}
			break;
		}
		extra_args(args, argidx, design);

		for (auto module : design->selected_modules()) {
			log("Running equiv_struct on module %s:\n", log_id(module));
			while (1) {
				EquivStructWorker worker(module, mode_fwd, mode_icells);
				if (worker.merge_count == 0)
					break;
			}
		}
	}
} EquivStructPass;

PRIVATE_NAMESPACE_END