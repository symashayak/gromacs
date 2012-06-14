/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2009, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 *
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 *
 * For more info, check our website at http://www.gromacs.org
 */
/*! \internal \file
 * \brief
 * Implements gmx::SelectionCollection.
 *
 * \author Teemu Murtola <teemu.murtola@cbr.su.se>
 * \ingroup module_selection
 */
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <cstdio>

#include <boost/shared_ptr.hpp>

#include "gromacs/legacyheaders/oenv.h"
#include "gromacs/legacyheaders/smalloc.h"
#include "gromacs/legacyheaders/xvgr.h"

#include "gromacs/options/basicoptions.h"
#include "gromacs/options/options.h"
#include "gromacs/selection/selection.h"
#include "gromacs/selection/selectioncollection.h"
#include "gromacs/utility/exceptions.h"
#include "gromacs/utility/file.h"
#include "gromacs/utility/gmxassert.h"
#include "gromacs/utility/messagestringcollector.h"
#include "gromacs/utility/stringutil.h"

#include "compiler.h"
#include "mempool.h"
#include "parser.h"
#include "poscalc.h"
#include "scanner.h"
#include "selectioncollection-impl.h"
#include "selelem.h"
#include "selhelp.h"
#include "selmethod.h"
#include "symrec.h"

namespace gmx
{

/********************************************************************
 * SelectionCollection::Impl
 */

SelectionCollection::Impl::Impl()
    : _options("selection", "Common selection control"),
      _debugLevel(0), _bExternalGroupsSet(false), _grps(NULL)
{
    _sc.root      = NULL;
    _sc.nvars     = 0;
    _sc.varstrs   = NULL;
    _sc.top       = NULL;
    gmx_ana_index_clear(&_sc.gall);
    _sc.mempool   = NULL;
    _sc.symtab    = NULL;

    _gmx_sel_symtab_create(&_sc.symtab);
    gmx_ana_selmethod_register_defaults(_sc.symtab);
}


SelectionCollection::Impl::~Impl()
{
    _gmx_selelem_free_chain(_sc.root);
    _sc.sel.clear();
    for (int i = 0; i < _sc.nvars; ++i)
    {
        sfree(_sc.varstrs[i]);
    }
    sfree(_sc.varstrs);
    gmx_ana_index_deinit(&_sc.gall);
    if (_sc.mempool)
    {
        _gmx_sel_mempool_destroy(_sc.mempool);
    }
    clearSymbolTable();
}


void
SelectionCollection::Impl::clearSymbolTable()
{
    if (_sc.symtab)
    {
        _gmx_sel_symtab_free(_sc.symtab);
        _sc.symtab = NULL;
    }
}


namespace
{

bool promptLine(File *infile, bool bInteractive, std::string *line)
{
    if (bInteractive)
    {
        fprintf(stderr, "> ");
    }
    if (!infile->readLine(line))
    {
        return false;
    }
    while(endsWith(*line, "\\\n"))
    {
        line->resize(line->length() - 2);
        if (bInteractive)
        {
            fprintf(stderr, "... ");
        }
        std::string buffer;
        // Return value ignored, buffer remains empty and works correctly
        // if there is nothing to read.
        infile->readLine(&buffer);
        line->append(buffer);
    }
    if (endsWith(*line, "\n"))
    {
        line->resize(line->length() - 1);
    }
    else if (bInteractive)
    {
        fprintf(stderr, "\n");
    }
    return line;
}

int runParserLoop(yyscan_t scanner, _gmx_sel_yypstate *parserState,
                  bool bInteractive)
{
    int status = YYPUSH_MORE;
    int prevToken = 0;
    do
    {
        YYSTYPE value;
        int token = _gmx_sel_yylex(&value, scanner);
        if (bInteractive)
        {
            if (token == 0)
            {
                break;
            }
            if (prevToken == CMD_SEP && token == CMD_SEP)
            {
                continue;
            }
            prevToken = token;
        }
        status = _gmx_sel_yypush_parse(parserState, token, &value, scanner);
    }
    while (status == YYPUSH_MORE);
    return status;
}

/*! \brief
 * Helper function that runs the parser once the tokenizer has been
 * initialized.
 *
 * \param[in,out] scanner Scanner data structure.
 * \param[in]     bStdIn  Whether to use a line-based reading
 *      algorithm designed for interactive input.
 * \param[in]     maxnr   Maximum number of selections to parse
 *      (if -1, parse as many as provided by the user).
 * \returns       Vector of parsed selections.
 * \throws        std::bad_alloc if out of memory.
 * \throws        InvalidInputError if there is a parsing error.
 *
 * Used internally to implement parseFromStdin(), parseFromFile() and
 * parseFromString().
 */
SelectionList runParser(yyscan_t scanner, bool bStdIn, int maxnr)
{
    boost::shared_ptr<void> scannerGuard(scanner, &_gmx_sel_free_lexer);
    gmx_ana_selcollection_t *sc = _gmx_sel_lexer_selcollection(scanner);

    MessageStringCollector errors;
    _gmx_sel_set_lexer_error_reporter(scanner, &errors);

    int oldCount = sc->sel.size();
    bool bOk = false;
    {
        boost::shared_ptr<_gmx_sel_yypstate> parserState(
                _gmx_sel_yypstate_new(), &_gmx_sel_yypstate_delete);
        if (bStdIn)
        {
            File &stdinFile(File::standardInput());
            bool bInteractive = _gmx_sel_is_lexer_interactive(scanner);
            std::string line;
            int status;
            while (promptLine(&stdinFile, bInteractive, &line))
            {
                line.append("\n");
                _gmx_sel_set_lex_input_str(scanner, line.c_str());
                status = runParserLoop(scanner, parserState.get(), true);
                if (status != YYPUSH_MORE)
                {
                    // TODO: Check if there is more input, and issue an
                    // error/warning if some input was ignored.
                    goto early_termination;
                }
                if (!errors.isEmpty() && bInteractive)
                {
                    fprintf(stderr, "%s", errors.toString().c_str());
                    errors.clear();
                }
            }
            status = _gmx_sel_yypush_parse(parserState.get(), 0, NULL,
                                           scanner);
early_termination:
            bOk = (status == 0);
        }
        else
        {
            int status = runParserLoop(scanner, parserState.get(), false);
            bOk = (status == 0);
        }
    }
    scannerGuard.reset();
    int nr = sc->sel.size() - oldCount;
    if (maxnr > 0 && nr != maxnr)
    {
        bOk = false;
        errors.append("Too few selections provided");
    }

    // TODO: Remove added selections from the collection if parsing failed?
    if (!bOk || !errors.isEmpty())
    {
        GMX_ASSERT(!bOk && !errors.isEmpty(), "Inconsistent error reporting");
        GMX_THROW(InvalidInputError(errors.toString()));
    }

    SelectionList result;
    SelectionDataList::const_iterator i;
    result.reserve(nr);
    for (i = sc->sel.begin() + oldCount; i != sc->sel.end(); ++i)
    {
        result.push_back(Selection(i->get()));
    }
    return result;
}

} // namespace


void SelectionCollection::Impl::resolveExternalGroups(
        t_selelem *root, MessageStringCollector *errors)
{

    if (root->type == SEL_GROUPREF)
    {
        bool bOk = true;
        if (_grps == NULL)
        {
            // TODO: Improve error messages
            errors->append("Unknown group referenced in a selection");
            bOk = false;
        }
        else if (root->u.gref.name != NULL)
        {
            char *name = root->u.gref.name;
            if (!gmx_ana_indexgrps_find(&root->u.cgrp, _grps, name))
            {
                // TODO: Improve error messages
                errors->append("Unknown group referenced in a selection");
                bOk = false;
            }
            else
            {
                sfree(name);
            }
        }
        else
        {
            if (!gmx_ana_indexgrps_extract(&root->u.cgrp, _grps,
                                           root->u.gref.id))
            {
                // TODO: Improve error messages
                errors->append("Unknown group referenced in a selection");
                bOk = false;
            }
        }
        if (bOk)
        {
            root->type = SEL_CONST;
            root->name = root->u.cgrp.name;
        }
    }

    t_selelem *child = root->child;
    while (child != NULL)
    {
        resolveExternalGroups(child, errors);
        child = child->next;
    }
}


/********************************************************************
 * SelectionCollection
 */

SelectionCollection::SelectionCollection()
    : _impl(new Impl)
{
}


SelectionCollection::~SelectionCollection()
{
}


Options &
SelectionCollection::initOptions()
{
    static const char * const debug_levels[]
        = {"no", "basic", "compile", "eval", "full", NULL};
    /*
    static const char * const desc[] = {
        "This program supports selections in addition to traditional",
        "index files. Use [TT]-select help[tt] for additional information,",
        "or type 'help' in the selection prompt.",
        NULL,
    };
    options.setDescription(desc);
    */

    Options &options = _impl->_options;
    const char *const *postypes = PositionCalculationCollection::typeEnumValues;
    options.addOption(StringOption("selrpos").enumValue(postypes)
                          .store(&_impl->_rpost).defaultValue(postypes[0])
                          .description("Selection reference positions"));
    options.addOption(StringOption("seltype").enumValue(postypes)
                          .store(&_impl->_spost).defaultValue(postypes[0])
                          .description("Default selection output positions"));
    GMX_RELEASE_ASSERT(_impl->_debugLevel >= 0 && _impl->_debugLevel <= 4,
                       "Debug level out of range");
    options.addOption(StringOption("seldebug").hidden(_impl->_debugLevel == 0)
                          .enumValue(debug_levels)
                          .defaultValue(debug_levels[_impl->_debugLevel])
                          .storeEnumIndex(&_impl->_debugLevel)
                          .description("Print out selection trees for debugging"));

    return _impl->_options;
}


void
SelectionCollection::setReferencePosType(const char *type)
{
    GMX_RELEASE_ASSERT(type != NULL, "Cannot assign NULL position type");
    // Check that the type is valid, throw if it is not.
    e_poscalc_t  dummytype;
    int          dummyflags;
    PositionCalculationCollection::typeFromEnum(type, &dummytype, &dummyflags);
    _impl->_rpost = type;
}


void
SelectionCollection::setOutputPosType(const char *type)
{
    GMX_RELEASE_ASSERT(type != NULL, "Cannot assign NULL position type");
    // Check that the type is valid, throw if it is not.
    e_poscalc_t  dummytype;
    int          dummyflags;
    PositionCalculationCollection::typeFromEnum(type, &dummytype, &dummyflags);
    _impl->_spost = type;
}


void
SelectionCollection::setDebugLevel(int debugLevel)
{
    _impl->_debugLevel = debugLevel;
}


void
SelectionCollection::setTopology(t_topology *top, int natoms)
{
    GMX_RELEASE_ASSERT(natoms > 0 || top != NULL,
        "The number of atoms must be given if there is no topology");
    // Get the number of atoms from the topology if it is not given.
    if (natoms <= 0)
    {
        natoms = top->atoms.nr;
    }
    gmx_ana_selcollection_t *sc = &_impl->_sc;
    // Do this first, as it allocates memory, while the others don't throw.
    gmx_ana_index_init_simple(&sc->gall, natoms, NULL);
    sc->pcc.setTopology(top);
    sc->top = top;
}


void
SelectionCollection::setIndexGroups(gmx_ana_indexgrps_t *grps)
{
    GMX_RELEASE_ASSERT(grps == NULL || !_impl->_bExternalGroupsSet,
                       "Can only set external groups once or clear them afterwards");
    _impl->_grps = grps;
    _impl->_bExternalGroupsSet = true;

    MessageStringCollector errors;
    t_selelem *root = _impl->_sc.root;
    while (root != NULL)
    {
        _impl->resolveExternalGroups(root, &errors);
        root = root->next;
    }
    if (!errors.isEmpty())
    {
        GMX_THROW(InvalidInputError(errors.toString()));
    }
}


bool
SelectionCollection::requiresTopology() const
{
    t_selelem   *sel;
    e_poscalc_t  type;
    int          flags;

    if (!_impl->_rpost.empty())
    {
        flags = 0;
        // Should not throw, because has been checked earlier.
        PositionCalculationCollection::typeFromEnum(_impl->_rpost.c_str(),
                                                    &type, &flags);
        if (type != POS_ATOM)
        {
            return true;
        }
    }
    if (!_impl->_spost.empty())
    {
        flags = 0;
        // Should not throw, because has been checked earlier.
        PositionCalculationCollection::typeFromEnum(_impl->_spost.c_str(),
                                                    &type, &flags);
        if (type != POS_ATOM)
        {
            return true;
        }
    }

    sel = _impl->_sc.root;
    while (sel)
    {
        if (_gmx_selelem_requires_top(sel))
        {
            return true;
        }
        sel = sel->next;
    }
    return false;
}


SelectionList
SelectionCollection::parseFromStdin(int nr, bool bInteractive)
{
    yyscan_t scanner;

    _gmx_sel_init_lexer(&scanner, &_impl->_sc, bInteractive, nr,
                        _impl->_bExternalGroupsSet,
                        _impl->_grps);
    return runParser(scanner, true, nr);
}


SelectionList
SelectionCollection::parseFromFile(const std::string &filename)
{
    yyscan_t scanner;

    File file(filename, "r");
    // TODO: Exception-safe way of using the lexer.
    _gmx_sel_init_lexer(&scanner, &_impl->_sc, false, -1,
                        _impl->_bExternalGroupsSet,
                        _impl->_grps);
    _gmx_sel_set_lex_input_file(scanner, file.handle());
    return runParser(scanner, false, -1);
}


SelectionList
SelectionCollection::parseFromString(const std::string &str)
{
    yyscan_t scanner;

    _gmx_sel_init_lexer(&scanner, &_impl->_sc, false, -1,
                        _impl->_bExternalGroupsSet,
                        _impl->_grps);
    _gmx_sel_set_lex_input_str(scanner, str.c_str());
    return runParser(scanner, false, -1);
}


void
SelectionCollection::compile()
{
    if (_impl->_sc.top == NULL && requiresTopology())
    {
        GMX_THROW(InconsistentInputError("Selection requires topology information, but none provided"));
    }
    if (!_impl->_bExternalGroupsSet)
    {
        setIndexGroups(NULL);
    }
    if (_impl->_debugLevel >= 1)
    {
        printTree(stderr, false);
    }

    SelectionCompiler compiler;
    compiler.compile(this);

    if (_impl->_debugLevel >= 1)
    {
        std::fprintf(stderr, "\n");
        printTree(stderr, false);
        std::fprintf(stderr, "\n");
        _impl->_sc.pcc.printTree(stderr);
        std::fprintf(stderr, "\n");
    }
    _impl->_sc.pcc.initEvaluation();
    if (_impl->_debugLevel >= 1)
    {
        _impl->_sc.pcc.printTree(stderr);
        std::fprintf(stderr, "\n");
    }
}


void
SelectionCollection::evaluate(t_trxframe *fr, t_pbc *pbc)
{
    _impl->_sc.pcc.initFrame();

    SelectionEvaluator evaluator;
    evaluator.evaluate(this, fr, pbc);

    if (_impl->_debugLevel >= 3)
    {
        std::fprintf(stderr, "\n");
        printTree(stderr, true);
    }
}


void
SelectionCollection::evaluateFinal(int nframes)
{
    SelectionEvaluator evaluator;
    evaluator.evaluateFinal(this, nframes);
}


void
SelectionCollection::printTree(FILE *fp, bool bValues) const
{
    t_selelem *sel;

    sel = _impl->_sc.root;
    while (sel)
    {
        _gmx_selelem_print_tree(fp, sel, bValues, 0);
        sel = sel->next;
    }
}


void
SelectionCollection::printXvgrInfo(FILE *out, output_env_t oenv) const
{
    if (output_env_get_xvg_format(oenv) != exvgNONE)
    {
        const gmx_ana_selcollection_t &sc = _impl->_sc;
        std::fprintf(out, "# Selections:\n");
        for (int i = 0; i < sc.nvars; ++i)
        {
            std::fprintf(out, "#   %s\n", sc.varstrs[i]);
        }
        for (size_t i = 0; i < sc.sel.size(); ++i)
        {
            std::fprintf(out, "#   %s\n", sc.sel[i]->selectionText());
        }
        std::fprintf(out, "#\n");
    }
}

// static
HelpTopicPointer
SelectionCollection::createDefaultHelpTopic()
{
    return createSelectionHelpTopic();
}

} // namespace gmx
